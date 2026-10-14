#!/usr/bin/env python3
"""Prepare an isolated RetroArch core override; inspect by default, never replace."""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-dir", required=True, type=Path,
                        help="RetroArch Settings > Directory > Configuration Files")
    parser.add_argument("--data-dir", required=True, type=Path,
                        help="Dedicated storage root for this fork")
    parser.add_argument("--write", action="store_true", help="Create the inspected profile")
    args = parser.parse_args()
    core_name = "mGBA REON"
    config_dir = args.config_dir.expanduser().resolve()
    data_dir = args.data_dir.expanduser().resolve() / "mgba-reon"
    profile = config_dir / core_name / (core_name + ".cfg")
    directories = {
        "savefile_directory": data_dir / "saves",
        "savestate_directory": data_dir / "states",
        "screenshot_directory": data_dir / "screenshots",
    }
    for path in [profile, *directories.values()]:
        if path.is_symlink() or path.resolve() != path:
            parser.error("Refusing an existing symbolic link in a destination")
        if any(char in str(path) for char in ['"', '\n', '\r']):
            parser.error("Destination cannot be represented in a RetroArch config")
    if profile.exists():
        parser.error("Profile already exists; preserve and review it instead of overwriting")
    text = "# mGBA REON only; enable automatic overrides in RetroArch.\n"
    for key, path in directories.items():
        text += f'{key} = "{path.as_posix()}"\n'
    text += ('savefiles_in_content_dir = "false"\n'
             'savestates_in_content_dir = "false"\n'
             'screenshots_in_content_dir = "false"\n'
             'config_save_on_exit = "false"\n')
    print(f"Profile: {profile}\n{text}")
    if not args.write:
        print("Inspection only. Add --write to create these fork-specific paths.")
        return
    for path in directories.values():
        path.mkdir(parents=True, exist_ok=True)
    profile.parent.mkdir(parents=True, exist_ok=True)
    with profile.open("x", encoding="utf-8", newline="\n") as output:
        output.write(text)
    print("Created. Verify automatic overrides and active directories before playing.")


if __name__ == "__main__":
    main()
