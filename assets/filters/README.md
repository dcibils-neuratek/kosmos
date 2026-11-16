# The filter lists blocking starts with

The three lists Privacy and Ad Blocking turns on by default
(`docs/adblock.md`), as the EasyList authors publish them, fetched on 16
November 2026. The build converts each into WebKit's content rules with
`user/lib/filters.lua`, and an image that carries WebKit serves them as
`/Kosmos/Filters`, so a machine blocks from its first boot. A machine
fetches newer copies into `/Home/Settings/Filters` itself (step B6).

| file | list | from | as of |
|---|---|---|---|
| `easylist.txt` | EasyList, adverts | <https://easylist.to/easylist/easylist.txt> | `Version: 202611161113` |
| `easyprivacy.txt` | EasyPrivacy, trackers | <https://easylist.to/easylist/easyprivacy.txt> | `Version: 202611161103` |
| `easylist-cookie.txt` | EasyList Cookie, the cookie banners | <https://secure.fanboy.co.nz/fanboy-cookiemonster.txt> | `Last modified: 2026-11-16 10:50 UTC` |

SHA-256:

    8976cdb7265fc4d5b40675ced673edb3f826638b493e9f7d22595e278f783921  easylist.txt
    042fefa0afb7ce2e61fe7f25ead218a98106725c3a578f724cd81a49ed2efba8  easyprivacy.txt
    9a572a906c48358d32d41f7ed06138511fb6f61a815340d157509168df7884c5  easylist-cookie.txt

**Unmodified**; `easylist-cookie.txt` is `fanboy-cookiemonster.txt`
renamed for the list's name. Licence: EasyList and EasyPrivacy are dual
licensed under the GNU General Public License version 3 and Creative
Commons Attribution-ShareAlike 3.0 Unported
(<https://easylist.to/pages/licence.html>), by The EasyList authors
(<https://easylist.to/>); EasyList Cookie says Creative Commons
Attribution 3.0 (<http://creativecommons.org/licenses/by/3.0/>), by
Fanboy and the EasyList authors.

Updating one is replacing its file with a newer copy and its line here.
