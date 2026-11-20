# Discord detectables: research, not a Trophy dependency

Investigated 2026-11-20. This is not legal advice or permission to reuse data.
The downloaded response was kept outside the repository for analysis only.
No entries, derived catalogue or full response are bundled with Trophy.

## Observed complete schema

`https://cdn.discordapp.com/detectables/games-v1.json` returned an array
of 25,387 objects. Exhaustive traversal of this response found these fields
and types (optional means absent from some entries; no nulls were observed):

```text
Game {
  id: string                     // Discord application identifier
  name: string
  aliases?: string[]
  executables?: {
    name: string                 // often a relative executable path
    os: string                   // win32 | darwin | linux in this snapshot
    arguments?: string
    is_launcher?: boolean
  }[]
  third_party_skus?: {
    distributor: string
    id?: string                  // some records omit it
  }[]
}
```

This is the observed schema, not a documented compatibility guarantee.
10,468 games had executable lists, containing 11,262 executable records:
11,182 `win32`, 72 `darwin`, **8 `linux`**. 9,310 executable names
contained slash/backslash path separators. They are detection patterns,
not necessarily installed Linux absolute paths or argv[0].
20,421 SKU records used distributor `steam`; the other observed
distributors were xbox, battlenet, google_play, uplay, epic, gog and
microsoft. A Steam SKU can provide an App ID but does not mean the
executable list includes a native Linux build.

Consequently this catalogue is potentially useful mostly for Windows
payload recognition under Proton/Wine, including non-Steam games. Windows
names/paths must not be applied indiscriminately to native ELF executables.
The sparse Linux data cannot replace native process/Steam/user evidence.
Launchers need separate handling; a catalogue match is not process
membership, a running instance, or an audio association.

## HTTP behaviour and update cadence

The research request returned `200`, `Cache-Control: public, max-age=3600`,
`Last-Modified: Fri, 20 Nov 2026 16:33:57 GMT`,
`ETag: W/"863730b8c9908754effaced53310a4a5"` and an internal versioned-file
metadata timestamp. Sending that exact `If-None-Match` returned **304**.
ETags/conditional retrieval therefore worked in this experiment.
One-hour cache freshness is not an update frequency or SLA. A single
observation cannot establish the actual publication schedule; it remains
unknown. Internal archive metadata is not an invitation to scrape archives.

Official developer [Application Resource documentation][applications]
provides application IDs, names, icons and related metadata, not a
documented public cross-platform executable catalogue API. Social/Rich
Presence APIs concern integrations, not a replacement for OS detection.
No official open-data licence or documented release cadence for this
detectables file was established.

## Rights and release decision

[Discord Terms of Service][terms] (effective September 29, 2025) reserve
Discord's content rights, restrict copying/derivative works of software and
services, and prohibit scraping without written consent.
[Developer Terms][developer] (effective July 8, 2024 in the retrieved
document) grant a limited purpose-bound API licence, reserve rights in API
Data, and require an appropriate licence or legal basis to use protected
data. Open-source components of the Discord client do not license this
database. Public accessibility, CORS and HTTP caching headers are technical
properties, not a redistribution licence.

The UK's [Copyright and Rights in Databases Regulations 1997][regulations]
must also be considered: copyright in original selection/arrangement is
distinct from database right in qualifying investment. Regulations 13–18
cover subsistence, ownership, extraction/re-utilisation, term and
qualification. Regulation 16 covers substantial extraction/re-utilisation
and repeated/systematic insubstantial extraction. Regulations 19–20 provide
limited lawful-user/exception provisions, not a general open-source
redistribution licence. Qualification, territorial application and the
particular proposed use require legal assessment; this report does **not**
assert that Discord necessarily holds a qualifying UK database right.

| Proposed Trophy use | Established permission? | Decision |
|---|---|---|
| Runtime query | No clear authorisation for this use | No detection fetcher |
| Local cache | Cache headers do not establish a legal licence | No automatic cache |
| Bundled copy | No redistribution licence found | Do not bundle |
| Individual extracted entries | Facts and protected expression differ; repeated extraction can accumulate | Do not assume permitted |
| Modified/derived catalogue | No derived-data redistribution basis established | Do not distribute |

Seek written permission defining query, storage, extraction and
redistribution rights, or qualified UK/IP legal advice, before enabling
such an integration. User opt-in alone would not cure licensing problems.
Discord could be a lead for independently investigating a game: confirm
executable facts through the publisher, licensed source code or actual
installed game, and record that independent provenance. Do not
systematically copy a substantial selection into a nominally “original”
Trophy database or assume a research exception permits product distribution.

[terms]: https://discord.com/terms
[developer]: https://discord.com/developers/docs/policies-and-agreements/developer-terms-of-service
[applications]: https://docs.discord.com/developers/resources/application
[regulations]: https://www.legislation.gov.uk/uksi/1997/3032/contents
