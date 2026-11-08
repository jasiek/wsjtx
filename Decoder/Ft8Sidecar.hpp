#ifndef FT8_SIDECAR_HPP__
#define FT8_SIDECAR_HPP__

#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QTimer>

#include "NonInheritingProcess.hpp"

//
// Ft8Sidecar - long-running external FT8 decoder
//
// When the environment variable WSJTX_FT8_DECODER holds a command
// line, WSJT-X starts that command once at startup and routes FT8
// decodes to it instead of jt9.  All other modes still use jt9.
//
// Protocol (v1) - see doc/ft8-sidecar-protocol.md for the full spec:
//
//   stdin  (WSJT-X -> decoder)
//     one JSON object per line ("hello", "decode", "quit"); a
//     "decode" line is immediately followed by `samples` * 2 bytes of
//     raw little-endian signed 16-bit mono audio at 12000 Hz.
//
//   stdout (decoder -> WSJT-X)
//     one JSON object per line: decodes {"seq","snr","dt","freq",
//     "msg",["ap"],["lowconf"]}, finished by {"seq":N,"done":true}.
//     Optional {"log":"..."} lines are written to the WSJT-X log.
//
//   stderr is forwarded to the WSJT-X log.
//
class Ft8Sidecar final
  : public QObject
{
  Q_OBJECT

public:
  static constexpr int protocol_version {1};
  static constexpr char const * env_var {"WSJTX_FT8_DECODER"};

  explicit Ft8Sidecar (QString const& command, QProcessEnvironment const&,
                       QObject * parent = nullptr);
  ~Ft8Sidecar ();

  QString const& command () const {return m_command;}

  // Start the process and send the "hello" message.
  void start (QJsonObject const& hello);

  // True when the process is running and can accept a request.
  bool available () const;
  bool busy () const {return m_busySeq >= 0;}

  // Send one decode request.  `utc` is the hhmmss used to format the
  // output lines.  Returns false (and sends nothing) if unavailable.
  bool decode (QJsonObject request, int utc, short const * samples,
               int nsamples, int timeout_ms);

  // Ask the decoder to exit; kills it if it hasn't after `wait_ms`.
  void stop (int wait_ms = 1000);

  // Format one decode exactly like jt9's FT8 output line
  // (Fortran: i6.6,i4,f5.1,i5,' ~ ',1x,a37,1x,a2).
  static QByteArray format_jt9_line (int utc, int snr, double dt, int freq,
                                     QString const& msg, int ap, bool lowconf);

Q_SIGNALS:
  // A decode, already formatted as a jt9 stdout line (no newline).
  void decoded (QByteArray const& jt9_line);
  // The current request is complete (done, timed out or crashed).
  void finished (int ndecodes);
  // The process can no longer be used; jt9 takes over FT8.
  void failed (QString const& reason);

private:
  void read_stdout ();
  void read_stderr ();
  void handle_line (QByteArray const&);
  void finish_request (char const * why);
  void write_json (QJsonObject const&);

  QString m_command;
  NonInheritingProcess m_proc;
  QTimer m_timeout;
  qint64 m_nextSeq {1};
  qint64 m_busySeq {-1};
  int m_busyUtc {0};
  int m_ndecodes {0};
  bool m_failed {false};
  bool m_stopping {false};
};

#endif
