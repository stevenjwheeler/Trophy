/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ProcessCensus.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <array>
#include <sys/stat.h>
#include <unistd.h>

namespace TrophyBackend {
namespace {
QByteArray read(const QString &path, qint64 limit = 262144) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  const auto bytes = file.read(limit + 1);
  return bytes.size() <= limit ? bytes : QByteArray{};
}
QString basename(QString path) {
  path.replace('\\', '/');
  return path.section('/', -1);
}
QString readLink(const QString &path) {
  // /proc links already contain the kernel's target path. QFileInfo's
  // symLinkTarget() additionally resolves filesystem path components, which
  // is unnecessary work for every process on every tick.
  std::array<char, 16384> bytes;
  const auto size = ::readlink(QFile::encodeName(path).constData(),
                               bytes.data(), bytes.size());
  if (size <= 0 || size == static_cast<ssize_t>(bytes.size()))
    return {};
  auto target = QString::fromLocal8Bit(bytes.data(), size);
  if (QDir::isRelativePath(target))
    target = QFileInfo(path).dir().absoluteFilePath(target);
  return target;
}
QString executablePath(const QString &base) {
  auto path = readLink(base + "/exe");
  if (path.endsWith(" (deleted)"))
    path.chop(10);
  return path;
}
bool wineLoader(const QString &executable) {
  const auto loader = basename(executable).toLower();
  return loader == "wine" || loader == "wine64" || loader == "wine-preloader" ||
         loader == "wine64-preloader";
}
QString wineArgument(const ProcessInfo &p) {
  if (!wineLoader(p.executable) || p.arguments.isEmpty())
    return {};
  auto candidate = p.arguments.first();
  if (basename(candidate).compare(basename(p.executable),
                                  Qt::CaseInsensitive) == 0 &&
      p.arguments.size() > 1)
    candidate = p.arguments.at(1);
  if (!candidate.endsWith(".exe", Qt::CaseInsensitive))
    return {};
  candidate.replace('\\', '/');
  return candidate;
}
QChar driveLetter(const QString &path) {
  if (path.size() < 3 || path[1] != ':' || path[2] != '/')
    return {};
  const auto letter = path[0].toLower();
  return letter >= QChar('a') && letter <= QChar('z') ? letter : QChar();
}
void resolveWineDrive(ProcessInfo &p) {
  const auto drive = driveLetter(wineArgument(p));
  if (drive.isNull() || p.winePrefix.isEmpty())
    return;
  const auto link = p.winePrefix + "/dosdevices/" + drive + ":";
  // Do not enumerate a prefix or follow mappings from payload() on the UI.
  // Resolve only the drive actually used by this process's payload.
  if (QFileInfo(link).isSymLink()) {
    const auto target = readLink(link);
    p.wineDrives.insert(drive, !target.isEmpty() && QDir(target).exists()
                                   ? QFileInfo(target).canonicalFilePath()
                                   : QString());
  }
}
} // namespace

QString ProcessInfo::payload() const {
  const QString loader = basename(executable).toLower();
  if (loader == "java" || loader == "mono" || loader == "dotnet" ||
      loader == "python" || loader.startsWith("python3") || loader == "sh" ||
      loader == "bash") {
    int index = 1;
    if (loader == "java" && arguments.value(index) == "-jar")
      ++index;
    const auto argument = arguments.value(index);
    if (!argument.isEmpty() && !argument.startsWith('-')) {
      return QDir::cleanPath(QDir::isAbsolutePath(argument)
                                 ? argument
                                 : directory + "/" + argument);
    }
    return {}; // Interpreter without a concrete script/assembly is not a game.
  }
  if (wineLoader(executable)) {
    QString candidate = wineArgument(*this);
    if (candidate.isEmpty())
      return {};
    const auto drive = driveLetter(candidate);
    if (!drive.isNull()) {
      if (wineDrives.contains(drive)) {
        const auto target = wineDrives.value(drive);
        if (target.isEmpty())
          return {};
        candidate = target + "/" + candidate.mid(3);
      } else if (drive == QChar('z')) {
        candidate = candidate.mid(2);
      } else if (drive == QChar('c') && !winePrefix.isEmpty()) {
        candidate = winePrefix + "/drive_c/" + candidate.mid(3);
      } else {
        return {}; // Unknown drive: never guess from CWD or the Steam App ID.
      }
    } else if (candidate.contains(':')) {
      return {}; // Drive-relative paths need Wine's per-drive CWD, not ours.
    }
    if (QDir::isRelativePath(candidate) && !candidate.contains(':') &&
        !directory.isEmpty())
      candidate = directory + "/" + candidate;
    return QDir::cleanPath(candidate);
  }
  return executable;
}

bool ProcessCensus::parseStat(const QByteArray &stat, ProcessInfo &process) {
  // comm can contain spaces and ')' characters. Fields after the last ')'
  // begin at state (field 3); starttime is field 22.
  const auto end = stat.lastIndexOf(')');
  if (end < 0)
    return false;
  const auto fields = stat.mid(end + 2).simplified().split(' ');
  if (fields.size() < 20 || fields[0] == "Z" || fields[0] == "X" ||
      fields[0] == "x")
    return false;
  bool parentOk = false, startOk = false;
  process.parent = fields[1].toLongLong(&parentOk);
  process.start = fields[19].toULongLong(&startOk);
  return parentOk && startOk && process.start > 0;
}

ProcessInfo ProcessCensus::inspect(qint64 pid, const QString &root) {
  ProcessInfo p;
  if (pid <= 0)
    return p;
  const QString base = root + "/" + QString::number(pid);
  struct stat owner{};
  if (::stat(QFile::encodeName(base).constData(), &owner) != 0 ||
      owner.st_uid != ::getuid())
    return p;
  p.pid = pid;
  if (!parseStat(read(base + "/stat", 8192), p))
    return {};
  p.executable = executablePath(base);
  p.directory = readLink(base + "/cwd");
  const auto command = read(base + "/cmdline");
  for (const auto &arg : command.split('\0'))
    if (!arg.isEmpty())
      p.arguments.append(QString::fromLocal8Bit(arg));
  for (const auto &entry : read(base + "/environ").split('\0')) {
    if (entry.startsWith("SteamAppId="))
      p.steamId = entry.mid(11).toUInt();
    if (!p.steamId && entry.startsWith("SteamGameId="))
      p.steamId = entry.mid(12).toUInt();
    if (entry.startsWith("WINEPREFIX="))
      p.winePrefix = QString::fromLocal8Bit(entry.mid(11));
  }
  resolveWineDrive(p);
  ProcessInfo after;
  if (p.executable.isEmpty() || !parseStat(read(base + "/stat", 8192), after) ||
      p.start != after.start || p.executable != executablePath(base))
    return {}; // Exit, reuse or exec during inspection: retry next scan.
  return p;
}

ProcessCensus::Snapshot ProcessCensus::scan(const QString &root) {
  Snapshot result;
  const auto entries = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const auto &entry : entries) {
    bool valid = false;
    const auto pid = entry.toLongLong(&valid);
    if (!valid || pid <= 0)
      continue;
    auto p = inspect(pid, root);
    if (p.pid)
      result.insert(pid, std::move(p));
  }
  return result;
}
} // namespace TrophyBackend
