/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>
#include <QStringList>
#include <QList>

namespace TrophyBackend {

struct DefaultAppInfo {
  QString key;
  QString artwork;
  // Exact known application IDs/executable names, NOT substring patterns.
  QStringList aliases;
  // Icon names are presentation only; never expand detection exclusions.
  QStringList icons;
};

inline const QList<DefaultAppInfo> &defaultAppCatalog() {
  static const QList<DefaultAppInfo> apps{
      {"xalia", "generic-game", {}, {}},
      {"steam", "steam", {"com.valvesoftware.steam"}, {}},
      {"discord", "discord", {"com.discordapp.discord"}, {}},
      {"obs", "obs", {"obs64", "obs-studio", "com.obsproject.studio"}, {"com.obsproject.Studio"}},
      {"unknown vulkan game", "generic-game", {}, {}},
      {"vkcube", "generic-cube", {"vkcube-wayland"}, {}},
      {"firefox", "firefox", {"org.mozilla.firefox"}, {}},
      {"chrome", "google-chrome", {"google-chrome", "google-chrome-stable", "com.google.chrome"}, {"google-chrome"}},
      {"chromium", "chromium", {"chromium-browser", "org.chromium.chromium"}, {"chromium-browser"}},
      {"brave", "brave", {"brave-browser", "com.brave.browser"}, {"brave-browser"}},
      {"msedge", "microsoft-edge", {"microsoft-edge", "microsoft-edge-stable", "com.microsoft.edge"}, {"microsoft-edge"}},
      {"opera", "opera", {"com.opera.opera"}, {}},
      {"vivaldi", "vivaldi", {"vivaldi-stable", "com.vivaldi.vivaldi"}, {}},
      {"librewolf", "librewolf", {"io.gitlab.librewolf-community"}, {}},
      {"thunderbird", "thunderbird", {"org.mozilla.thunderbird"}, {}},
      {"slack", "slack", {"com.slack.slack"}, {}},
      {"telegram", "telegram", {"telegram-desktop", "org.telegram.desktop"}, {}},
      {"signal", "signal-desktop", {"signal-desktop", "org.signal.signal"}, {"org.signal.Signal"}},
      {"element", "element", {"element-desktop", "im.riot.riot"}, {"element-desktop"}},
      {"zoom", "zoom", {"us.zoom.zoom"}, {"Zoom", "us.zoom.Zoom"}},
      {"spotify", "spotify", {"com.spotify.client"}, {"spotify-client"}},
      {"vlc", "vlc", {"org.videolan.vlc"}, {}},
      {"mpv", "mpv", {"io.mpv.mpv"}, {}},
      {"gimp", "gimp", {"org.gimp.gimp"}, {}},
      {"krita", "krita", {"org.kde.krita"}, {}},
      {"inkscape", "inkscape", {"org.inkscape.inkscape"}, {}},
      {"blender", "blender", {"org.blender.blender"}, {}},
      {"code", "visual-studio-code", {"com.visualstudio.code", "visual-studio-code"}, {"vscode"}},
      {"konsole", "konsole", {"org.kde.konsole"}, {"utilities-terminal"}},
      {"gnome-terminal", "gnome-terminal", {"org.gnome.terminal", "gnome-terminal-server"}, {"org.gnome.Terminal", "utilities-terminal"}},
      {"alacritty", "alacritty", {"org.alacritty.alacritty"}, {"Alacritty"}},
      {"kitty", "kitty", {}, {}},
      {"foot", "foot", {"footclient"}, {}},
      {"wezterm", "wezterm", {"org.wezfurlong.wezterm", "wezterm-gui"}, {"org.wezfurlong.wezterm"}},
      {"dolphin", "dolphin", {"org.kde.dolphin"}, {}},
      {"nautilus", "nautilus", {"org.gnome.nautilus"}, {"org.gnome.Nautilus", "system-file-manager"}},
      {"thunar", "thunar", {"org.xfce.thunar"}, {"Thunar", "system-file-manager"}},
  };
  return apps;
}

inline const DefaultAppInfo *defaultAppInfo(const QString &normalizedKey) {
  for (const auto &app : defaultAppCatalog()) {
    if (app.key == normalizedKey || app.aliases.contains(normalizedKey))
      return &app;
  }
  return nullptr;
}

// Only veto operations use this mapping. Whitelists, stable game identities,
// portal tokens and arbitrary custom keys retain their original semantics.
inline QString defaultExclusionKey(const QString &normalizedKey) {
  const auto *app = defaultAppInfo(normalizedKey);
  return app ? app->key : normalizedKey;
}

} // namespace TrophyBackend
