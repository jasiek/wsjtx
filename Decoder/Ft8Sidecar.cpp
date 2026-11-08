#include "Ft8Sidecar.hpp"

#include <algorithm>

#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <QtGlobal>

namespace
{
  // Keep a value inside what a fixed-width Fortran field can show so
  // that the column positions DecodedText relies on never shift.
  template<typename T>
  T clamp_field (T v, T lo, T hi) {return std::max (lo, std::min (hi, v));}

  QStringList split_command (QString const& command)
  {
#if QT_VERSION >= QT_VERSION_CHECK (5, 15, 0)
    return QProcess::splitCommand (command);
#else
    return command.split (' ', QString::SkipEmptyParts);
#endif
  }
}

Ft8Sidecar::Ft8Sidecar (QString const& command, QProcessEnvironment const& env,
                        QObject * parent)
  : QObject {parent}
  , m_command {command}
{
  m_proc.setProcessEnvironment (env);
  m_proc.setReadChannel (QProcess::StandardOutput);
  m_timeout.setSingleShot (true);

  connect (&m_proc, &QProcess::readyReadStandardOutput, this, &Ft8Sidecar::read_stdout);
  connect (&m_proc, &QProcess::readyReadStandardError, this, &Ft8Sidecar::read_stderr);
  connect (&m_timeout, &QTimer::timeout, this, [this] {
      qWarning () << "FT8 sidecar: request" << m_busySeq << "timed out";
      finish_request ("timeout");
    });
  connect (&m_proc, &QProcess::errorOccurred, this, [this] (QProcess::ProcessError error) {
      if (m_stopping) return;
      if (QProcess::FailedToStart == error || QProcess::Crashed == error)
        {
          m_failed = true;
          if (busy ()) finish_request ("process error");
          Q_EMIT failed (tr ("FT8 decoder \"%1\": %2").arg (m_command).arg (m_proc.errorString ()));
        }
    });
  connect (&m_proc, static_cast<void (QProcess::*) (int, QProcess::ExitStatus)> (&QProcess::finished),
           this, [this] (int exit_code, QProcess::ExitStatus status) {
      read_stdout ();                       // drain anything left
      read_stderr ();
      if (m_stopping) return;
      bool was_failed = m_failed;
      m_failed = true;
      if (busy ()) finish_request ("process exited");
      if (!was_failed)          // errorOccurred(Crashed) already reported it
        {
          Q_EMIT failed (tr ("FT8 decoder \"%1\" exited (%2, code %3)")
                         .arg (m_command)
                         .arg (QProcess::NormalExit == status ? "normal" : "crash")
                         .arg (exit_code));
        }
    });
}

Ft8Sidecar::~Ft8Sidecar ()
{
  stop (0);
}

void Ft8Sidecar::start (QJsonObject const& hello)
{
  auto args = split_command (m_command);
  if (args.isEmpty ())
    {
      m_failed = true;
      Q_EMIT failed (tr ("%1 is set but empty").arg (env_var));
      return;
    }
  auto program = args.takeFirst ();
  qInfo () << "FT8 sidecar: starting" << program << args;
  m_proc.start (program, args, QIODevice::ReadWrite | QIODevice::Unbuffered);
  if (!m_proc.waitForStarted (5000)) return; // errorOccurred() reports it

  QJsonObject msg {hello};
  msg["type"] = "hello";
  msg["v"] = protocol_version;
  write_json (msg);
}

bool Ft8Sidecar::available () const
{
  return !m_failed && !m_stopping && QProcess::NotRunning != m_proc.state ();
}

bool Ft8Sidecar::decode (QJsonObject request, int utc, short const * samples,
                         int nsamples, int timeout_ms)
{
  if (!available () || busy ()) return false;

  m_busySeq = m_nextSeq++;
  m_busyUtc = utc;
  m_ndecodes = 0;

  request["type"] = "decode";
  request["v"] = protocol_version;
  request["seq"] = m_busySeq;
  request["sample_rate"] = 12000;
  request["samples"] = nsamples;
  write_json (request);
  // Audio is raw host-order int16; every platform WSJT-X ships on
  // is little-endian, which is what the protocol specifies.
  m_proc.write (reinterpret_cast<char const *> (samples), qint64 (nsamples) * 2);

  m_timeout.start (timeout_ms);
  return true;
}

void Ft8Sidecar::stop (int wait_ms)
{
  if (QProcess::NotRunning == m_proc.state ()) return;
  m_stopping = true;
  m_timeout.stop ();
  write_json (QJsonObject {{"type", "quit"}, {"v", protocol_version}});
  m_proc.closeWriteChannel ();
  if (!wait_ms || !m_proc.waitForFinished (wait_ms))
    {
      m_proc.kill ();
      m_proc.waitForFinished (500);
    }
}

void Ft8Sidecar::write_json (QJsonObject const& obj)
{
  if (QProcess::Running != m_proc.state ()) return;
  m_proc.write (QJsonDocument {obj}.toJson (QJsonDocument::Compact) + '\n');
}

void Ft8Sidecar::read_stdout ()
{
  while (m_proc.canReadLine ())
    {
      auto line = m_proc.readLine ().trimmed ();
      if (!line.isEmpty ()) handle_line (line);
    }
}

void Ft8Sidecar::read_stderr ()
{
  auto text = m_proc.readAllStandardError ();
  for (auto const& line : text.split ('\n'))
    {
      auto t = line.trimmed ();
      if (!t.isEmpty ()) qInfo ().noquote () << "FT8 sidecar stderr:" << QString::fromUtf8 (t);
    }
}

void Ft8Sidecar::handle_line (QByteArray const& line)
{
  QJsonParseError err;
  auto doc = QJsonDocument::fromJson (line, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject ())
    {
      qWarning ().noquote () << "FT8 sidecar: ignoring non-JSON stdout line:" << QString::fromUtf8 (line);
      return;
    }
  auto const obj = doc.object ();  // const: operator[] must not insert keys

  if (obj.contains ("log"))
    {
      qInfo ().noquote () << "FT8 sidecar:" << obj["log"].toString ();
      if (obj.size () == 1) return;
    }

  if (!busy ()) return;         // late reply to a timed-out request
  if (obj.contains ("seq") && obj["seq"].toVariant ().toLongLong () != m_busySeq) return;

  if (obj["done"].toBool ())
    {
      finish_request (nullptr);
      return;
    }

  if (!obj.contains ("msg")) return;
  ++m_ndecodes;
  Q_EMIT decoded (format_jt9_line (m_busyUtc,
                                   obj["snr"].toInt (),
                                   obj["dt"].toDouble (),
                                   qRound (obj["freq"].toDouble ()),
                                   obj["msg"].toString (),
                                   obj["ap"].toInt (),
                                   obj["lowconf"].toBool ()));
}

void Ft8Sidecar::finish_request (char const * why)
{
  if (!busy ()) return;
  m_timeout.stop ();
  if (why) qWarning () << "FT8 sidecar: request" << m_busySeq << "ended early:" << why;
  m_busySeq = -1;
  Q_EMIT finished (m_ndecodes);
}

QByteArray Ft8Sidecar::format_jt9_line (int utc, int snr, double dt, int freq,
                                        QString const& msg, int ap, bool lowconf)
{
  QByteArray text = msg.trimmed ().toLatin1 ().left (37);
  text = text.leftJustified (37, ' ');
  if (lowconf) text[36] = '?';

  QByteArray annot {"  "};
  if (ap > 0) annot = "a" + QByteArray::number (std::min (ap, 9));

  return QString::asprintf ("%06d%4d%5.1f%5d ~  ",
                            clamp_field (utc, 0, 999999),
                            clamp_field (snr, -999, 9999),
                            clamp_field (dt, -99.9, 999.9),
                            clamp_field (freq, 0, 99999)).toLatin1 ()
    + text + ' ' + annot;
}
