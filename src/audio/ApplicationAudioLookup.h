/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "ApplicationAudioIdentity.h"
#include <QMap>
#include <pipewire/keys.h>
#include <spa/utils/dict.h>

namespace TrophyBackend::detail {

// Lookup-local snapshots. Registry dictionaries seed the snapshot; an info
// event with PROPS set replaces the whole dictionary (as pw_node_info_merge
// does). State/parameter-only events must not erase it or freeze evaluation.
struct AudioLookupProperties {
  QMap<QString, QString> values;

  void update(const spa_dict *props) {
    if (!props) return;
    const spa_dict_item *item;
    spa_dict_for_each(item, props) {
      const QString key = QString::fromUtf8(item->key);
      if (item->value) values[key] = QString::fromUtf8(item->value);
      else values.remove(key);
    }
  }

  void updateInfo(const spa_dict *props, bool propertiesChanged) {
    if (!propertiesChanged) return;
    values.clear();
    update(props);
  }

  QString get(const char *key) const { return values.value(QString::fromUtf8(key)); }

  bool matches(uint32_t pid, const QString &hint, bool exactBinary) const {
    if (pid) {
      bool valid = false;
      const auto candidate = get(PW_KEY_APP_PROCESS_ID).toUInt(&valid);
      return valid && candidate == pid;
    }
    if (hint.isEmpty()) return false;
    if (exactBinary) return matchesAudioBinary(get(PW_KEY_APP_PROCESS_BINARY), hint);
    for (const char *key : {PW_KEY_APP_PROCESS_BINARY, PW_KEY_APP_NAME,
                            PW_KEY_NODE_NAME, PW_KEY_APP_ID})
      if (get(key).contains(hint, Qt::CaseInsensitive)) return true;
    return false;
  }
};

inline AudioLookupProperties gameAudioLookupProperties(
    const AudioLookupProperties &node, const AudioLookupProperties &client) {
  auto result = node;
  for (const char *key : {PW_KEY_APP_PROCESS_ID, PW_KEY_APP_PROCESS_BINARY}) {
    if (!node.get(key).isEmpty() && !client.get(key).isEmpty() &&
        node.get(key).compare(client.get(key), Qt::CaseInsensitive) != 0)
      return result; // A broker's process identity is not the stream's owner.
  }
  // Node identity wins. Never inherit routing fields (name/serial/class) or
  // weaken strict Voice verification with a broker client's identity.
  for (const char *key : {PW_KEY_APP_PROCESS_ID, PW_KEY_APP_PROCESS_BINARY,
                          PW_KEY_APP_NAME, PW_KEY_APP_ID}) {
    if (node.get(key).isEmpty() && !client.get(key).isEmpty())
      result.values[QString::fromUtf8(key)] = client.get(key);
  }
  return result;
}
} // namespace TrophyBackend::detail
