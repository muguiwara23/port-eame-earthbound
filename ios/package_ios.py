#!/usr/bin/env python3
"""Port/eame: package the iOS build of Phase Distorter as an unsigned .ipa.

Writes Payload/Phase Distorter.app (the executable, Info.plist and the
license notices) and zips it under the output folder. The app holds no ROM,
asset pack or save: the player puts their own ROM in the app's Documents
folder with the Files app (UIFileSharingEnabled), and it is imported there on
the first launch. The .ipa is NOT signed; the player's own tool signs it."""
import argparse
import hashlib
import json
import plistlib
import shutil
import stat
import subprocess
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APP_NAME = "Phase Distorter"
EXECUTABLE = "PhaseDistorter"
MINIMUM_IOS = "16.0"
IOS_MINIMUM = "16.0"
FORBIDDEN = (".sfc", ".smc", ".ebpak", ".srm", ".state")


def info_plist(version, build, bundle_id):
    landscape = ["UIInterfaceOrientationLandscapeLeft", "UIInterfaceOrientationLandscapeRight"]
    return dict(
        CFBundleDevelopmentRegion="en",
        CFBundleName=APP_NAME,
        CFBundleDisplayName="EarthBound",
        CFBundleIdentifier=bundle_id,
        CFBundleExecutable=EXECUTABLE,
        CFBundlePackageType="APPL",
        CFBundleInfoDictionaryVersion="6.0",
        CFBundleShortVersionString=version,
        CFBundleVersion=build,
        CFBundleSupportedPlatforms=["iPhoneOS"],
        LSRequiresIPhoneOS=True,
        MinimumOSVersion=MINIMUM_IOS,
        UIDeviceFamily=[1, 2],
        UIRequiredDeviceCapabilities=["arm64"],
        UISupportedInterfaceOrientations=landscape,
        **{"UISupportedInterfaceOrientations~ipad": landscape},
        UIRequiresFullScreen=True,
        UIStatusBarHidden=True,
        UIViewControllerBasedStatusBarAppearance=False,
        UILaunchScreen={},
        # The scene life cycle iOS 27 requires; SDL3's UIKit backend supplies
        # this delegate (as in Port/eame's Yu-Gi-Oh! Forbidden Memories).
        UIApplicationSceneManifest=dict(
            UIApplicationSupportsMultipleScenes=False,
            UISceneConfigurations=dict(
                UIWindowSceneSessionRoleApplication=[
                    dict(
                        UISceneConfigurationName="SDLSceneConfiguration",
                        UISceneDelegateClassName="SDLUIKitSceneDelegate",
                    )
                ]
            ),
        ),
        # Documents shows in the Files app: the ROM, the imported packs, saves.
        UIFileSharingEnabled=True,
        LSSupportsOpeningDocumentsInPlace=True,
        UIApplicationSupportsIndirectInputEvents=True,
        GCSupportsControllerUserInteraction=True,
        ITSAppUsesNonExemptEncryption=False,
    )


def stage_icon(app, icon, info):
    """The app's icon from one square picture: actool makes the asset
    catalog and the sizes iPhone and iPad want (Info.plist's CFBundleIcons);
    without actool, sips makes the sizes and the plist names them."""
    # Beside the stage, not in it: only Payload/<app> goes into the .ipa.
    work = app.parent.parent.parent / "icon-work"
    shutil.rmtree(work, ignore_errors=True)
    catalog = work / "Assets.xcassets"
    appicon = catalog / "AppIcon.appiconset"
    appicon.mkdir(parents=True)
    # A 1024 x 1024 PNG (the private icon is one already, RGB with no
    # transparency); sips only resizes or converts what is not.
    picture = appicon / "icon.png"
    shutil.copyfile(icon, picture)
    if icon.suffix.lower() != ".png" or icon.read_bytes()[16:24] != (1024).to_bytes(4, "big") * 2:
        subprocess.run(
            ["sips", "-s", "format", "png", "-z", "1024", "1024", str(icon), "--out", str(picture)],
            check=True, stdout=subprocess.DEVNULL,
        )
    (catalog / "Contents.json").write_text('{"info":{"author":"xcode","version":1}}')
    (appicon / "Contents.json").write_text(json.dumps({
        "images": [{"filename": "icon.png", "idiom": "universal", "platform": "ios", "size": "1024x1024"}],
        "info": {"author": "xcode", "version": 1},
    }))
    partial = work / "partial.plist"
    made = subprocess.run(
        ["xcrun", "actool", str(catalog), "--compile", str(app), "--platform", "iphoneos",
         "--minimum-deployment-target", IOS_MINIMUM, "--app-icon", "AppIcon",
         "--target-device", "iphone", "--target-device", "ipad",
         "--output-partial-info-plist", str(partial), "--output-format", "human-readable-text"],
        check=False,
    )
    if made.returncode == 0 and partial.is_file():
        info.update(plistlib.loads(partial.read_bytes()))
        print("icon: asset catalog by actool:", sorted(p.name for p in app.glob("AppIcon*")) + (["Assets.car"] if (app / "Assets.car").is_file() else []))
        return
    print("icon: actool failed; plain PNG icons")
    sizes = {"AppIcon60x60@2x.png": 120, "AppIcon60x60@3x.png": 180,
             "AppIcon76x76@2x~ipad.png": 152, "AppIcon83.5x83.5@2x~ipad.png": 167}
    for name, size in sizes.items():
        subprocess.run(["sips", "-z", str(size), str(size), str(appicon / "icon.png"), "--out", str(app / name)],
                       check=True, stdout=subprocess.DEVNULL)
    info["CFBundleIcons"] = {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60"]}}
    info["CFBundleIcons~ipad"] = {"CFBundlePrimaryIcon": {"CFBundleIconFiles": ["AppIcon60x60", "AppIcon76x76", "AppIcon83.5x83.5"]}}


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--version", default="0.5-ios-dev")
    parser.add_argument("--bundle-id", default="org.porteame.earthbound")
    parser.add_argument("--sdl-license", type=Path, required=True)
    parser.add_argument("--icon", type=Path, help="A square picture for the app's icon (kept out of the repository)")
    options = parser.parse_args()

    out = options.out.resolve()
    stage = out / "stage"
    shutil.rmtree(stage, ignore_errors=True)
    app = stage / "Payload" / f"{APP_NAME}.app"
    app.mkdir(parents=True)
    executable = app / EXECUTABLE
    shutil.copyfile(options.binary, executable)
    executable.chmod(0o755)
    # No signature at all: a stale or ad hoc one only confuses signing tools.
    if shutil.which("codesign"):
        subprocess.run(["codesign", "--remove-signature", str(executable)], check=False)
    (app / "PkgInfo").write_text("APPL????")

    licenses = app / "licenses"
    licenses.mkdir()
    for name, source in {
        "Phase-Distorter-LICENSE.txt": ROOT / "LICENSE",
        "Phase-Distorter-NOTICE.md": ROOT / "NOTICE.md",
        "SDL3.txt": options.sdl_license,
        "Dear-ImGui.txt": ROOT / "cpp/external/imgui/LICENSE.txt",
        "snes_spc-LGPL-2.1.txt": ROOT / "cpp/external/spc_dsp/LICENSE",
        "crt-lottes-fast.md": ROOT / "cpp/external/crt-lottes-fast/README.md",
    }.items():
        if not source.is_file():
            raise SystemExit(f"Missing license notice: {source}")
        shutil.copyfile(source, licenses / name)

    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    build = str(int(revision[:7], 16)) if revision else "1"
    numeric = "".join(c for c in options.version.split("-")[0] if c.isdigit() or c == ".") or "0.5"
    info = info_plist(numeric, build, options.bundle_id)
    if options.icon:
        stage_icon(app, options.icon.resolve(), info)
    (app / "Info.plist").write_bytes(plistlib.dumps(info, fmt=plistlib.FMT_BINARY))

    archive = out / f"PhaseDistorter-{options.version}-ios-arm64-unsigned.ipa"
    archive.unlink(missing_ok=True)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as bundle:
        for path in sorted(stage.rglob("*")):
            if path.is_dir():
                continue
            entry = zipfile.ZipInfo.from_file(path, path.relative_to(stage).as_posix())
            mode = 0o755 if path == executable else 0o644
            entry.external_attr = (stat.S_IFREG | mode) << 16
            entry.compress_type = zipfile.ZIP_DEFLATED
            bundle.writestr(entry, path.read_bytes())

    # Check the archive as an installer reads it.
    with zipfile.ZipFile(archive) as bundle:
        names = bundle.namelist()
        prefix = f"Payload/{APP_NAME}.app/"
        if not all(name.startswith(prefix) for name in names):
            raise SystemExit("The .ipa holds files outside Payload/<app>")
        read = plistlib.loads(bundle.read(prefix + "Info.plist"))
        if read["CFBundleExecutable"] != EXECUTABLE or prefix + EXECUTABLE not in names:
            raise SystemExit("The .ipa's Info.plist does not name its executable")
        forbidden = [n for n in names if n.lower().endswith(FORBIDDEN)]
        if forbidden:
            raise SystemExit(f"The .ipa must hold no ROM, pack or save: {forbidden}")
    checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
    Path(str(archive) + ".sha256").write_text(f"{checksum}  {archive.name}\n")
    print(f"{archive} ({archive.stat().st_size} bytes, {len(names)} files)")
    print(f"sha256 {checksum}")
    print(f"bundle id {options.bundle_id}, version {numeric} ({build}), iOS {MINIMUM_IOS}+, unsigned")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
