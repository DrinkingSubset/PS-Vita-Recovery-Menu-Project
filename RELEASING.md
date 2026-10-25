# Releasing a new build (copy-paste guide)

## 1. Build
```bash
cd ~/Desktop/VitaRecovery
bash build.sh bump      # raises VERSION by one step (skip if you already set it)
bash build.sh clean     # or plain: bash build.sh
```
Result: `VitaRecovery_vX.Y.Z.vpk` in the same folder.

## 2. Test on the Vita
Install the VPK with VitaShell, open the app, check the title bar shows the new version.

## 3. Update the changelog
Add a new block at the top of `CHANGELOG.md` (copy the template below).

## 4. Push the source to GitHub (first time)
```bash
cd ~/Desktop/VitaRecovery
git init                                  # only if this folder is not a repo yet
git remote add origin https://github.com/DrinkingSubset/PS-Vita-Recovery-Menu-Project.git
git add -A
git commit -m "v1.1.1: Check for Updates"
git branch -M main
git push -u origin main                   # may ask you to sign in / use a token
```
If the repo already has files, run `git pull origin main --allow-unrelated-histories` first.
Next time only: `git add -A && git commit -m "vX.Y.Z: ..." && git push`.
Do not commit `build/` or `*.vpk` (a `.gitignore` with those two lines is enough).

## 5. Publish the release (this is what the in-app update check reads)
1. GitHub > the repo > **Releases** > **Draft a new release**.
2. **Choose a tag**: type `vX.Y.Z` (exactly the version in `VERSION`, with a leading v) and create it.
3. **Title**: `vX.Y.Z`.
4. **Description**: paste the version's block from `CHANGELOG.md`.
5. **Attach binaries**: drag in `VitaRecovery_vX.Y.Z.vpk`. Attach exactly one `.vpk`.
6. Leave **pre-release** and **draft** unticked.
7. **Publish release**.

## 6. Check it
On the Vita: Main menu > Check for Updates. An older build should say
"A newer version is available"; the same build says "You are up to date".

## Changelog block template
```
## vX.Y.Z
- New: ...
- Changed: ...
- Fixed: ...
- Known issues: ...
```
