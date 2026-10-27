# Releasing the Clownsec Player

Since build 61024.4 the player updates itself: it reads `latest.txt` from its channel's GitHub
release, and on UPDATE downloads the CIA (or `.3dsx` under the Homebrew Launcher) that sits beside
it, checks its MD5, installs it over itself and relaunches. Every release has to keep that working.

## Rules

1. **`latest.txt` is two lines:** the build tag, then the MD5 of the exact `.cia` being uploaded.
   ```
   61024.4
   d84c0a3671538fc06f6e8c93da0a3c73
   ```
   Wrong MD5 = every user's update fails with "The download was damaged". Missing line 2 still
   installs (AM checks the CIA's own hashes), but always publish it.
2. **Upload the `.cia` and `.3dsx` first, `latest.txt` LAST.** `latest.txt` is what makes players
   offer the update; if it goes up first, anyone checking in between downloads the OLD CIA, and
   its MD5 won't match.
3. **Asset names and releases are fixed per channel** (the app derives the URL from its TitleID):

   | Channel | Release      | Assets                                                  |
   |---------|--------------|---------------------------------------------------------|
   | stable  | `v1.0`       | `clownsec_player.cia`, `clownsec_player.3dsx`, `latest.txt` |
   | beta    | `v1.1.0-beta`| `clownsec_player_beta.cia`, `clownsec_player_beta.3dsx`, `latest.txt` |
   | mp4     | `mp4-beta`   | `clownsec_player_mp4.cia`, `clownsec_player_mp4.3dsx`, `latest.txt` |

4. **Never change a channel's UniqueID** in `cia*.rsf` (0xf8a2c / 0xf8a2d / 0xf8a2e), and keep
   `am:net` in its ServiceAccessControl. A new UniqueID installs as a separate app instead of
   upgrading; without `am:net` the install fails.
5. **The build tag must go up.** The check compares `MAJOR.MINOR` numerically (`61024.4` >
   `61024.3`); `buildnum.sh` handles this as long as the tag is pinned for `make clean` and the build.
6. **Never ship a stable build made with `UPDATE_TEST_URL` set.** It points the update check at
   the test folder, and those users would never see another real update. After building, check:
   `strings clownsec_player.3dsx | grep -c clownsec.com/test` must print `0`.

## Stable release steps

```sh
cd app
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
unset UPDATE_TEST_URL
export BUILD_TAG=$(./buildnum.sh)
make clean BUILD_TAG=$BUILD_TAG && ./build_cia.sh
strings clownsec_player.3dsx | grep -c clownsec.com/test        # must be 0
printf '%s\n%s\n' "$BUILD_TAG" "$(md5 -q clownsec_player.cia)" > latest.txt
cp latest.txt ../latest.txt
gh release upload v1.0 clownsec_player.cia clownsec_player.3dsx --clobber
gh release upload v1.0 latest.txt --clobber                      # last
curl -sL https://github.com/brainphreak/clownsec-moflex-player/releases/download/v1.0/clownsec_player.cia | md5
```
Then commit `latest.txt -> <build>`, tag `prod-<build>`, and push
`refs/heads/mp4-beta:refs/heads/mp4-beta` plus the tag.

## Testing the updater itself

Build two test builds with `UPDATE_TEST_URL=https://clownsec.com/test/latest.txt` (make clean
each time). Install the older one from a QR to `https://clownsec.com/test/clownsec_player_test.cia`;
put the newer one at `/var/www/html/clownsec/test/clownsec_player.cia` (+ `.3dsx`) with a two-line
`latest.txt` beside it. Open the player: it should offer and install the newer build. Delete the
test files afterwards.

Users on builds older than 61024.4 have no self-updater and need one last QR install.
