#!/usr/bin/env python3

import argparse
import subprocess
from pathlib import Path

from west.commands import WestCommand


class YkbBuild(WestCommand):

    def __init__(self):
        super().__init__(
            "ykb-build",
            "smart YKB board build",
            "Build a YKB board, expanding split boards into left/right builds.",
            accepts_unknown_args=True,
        )

    def do_add_parser(self, parser_adder):
        parser = parser_adder.add_parser(
            self.name,
            help=self.help,
            description=self.description,
            formatter_class=argparse.RawDescriptionHelpFormatter,
        )
        parser.add_argument(
            "board",
            help="Board name or fully qualified board target",
        )
        parser.add_argument(
            "-s",
            "--source-dir",
            default="app",
            help=
            "Application source directory relative to the repository root (default: %(default)s)",
        )
        parser.add_argument(
            "-d",
            "--build-dir",
            help="Optional build directory for single-target builds only",
        )
        parser.add_argument(
            "-p",
            "--pristine",
            action="store_true",
            help="Build with `--pristine`",
        )
        parser.add_argument(
            "--debug",
            action="store_true",
            help="Apply the debug config fragment at app/conf/debug.conf",
        )
        parser.add_argument(
            "-n",
            "--dry-run",
            action="store_true",
            help=
            "Print the resolved board and build command without executing it",
        )
        return parser

    def do_run(self, args, unknown_args):
        repo_root = Path(__file__).resolve().parents[2]
        source_dir = Path(args.source_dir)
        if not source_dir.is_absolute():
            source_dir = (repo_root / source_dir).resolve()

        if not source_dir.exists():
            self.die(f"source directory does not exist: {source_dir}")

        passthrough_args = list(unknown_args)
        if passthrough_args and passthrough_args[0] == "--":
            passthrough_args = passthrough_args[1:]

        build_targets = self._resolve_build_targets(repo_root, args.board,
                                                    args.build_dir)

        if len(build_targets) > 1 and args.build_dir:
            self.die("--build-dir cannot be used with split board auto-expansion")

        commands = [
            self._build_command(source_dir, board, build_dir, args.pristine,
                                args.debug, passthrough_args)
            for board, build_dir in build_targets
        ]

        for board, build_dir, cmd in commands:
            self.inf(f"resolved board: {board}")
            self.inf(f"build dir: {build_dir}")
            self.inf("command: " + " ".join(cmd))

        if args.dry_run:
            return

        for board, build_dir, cmd in commands:
            result = subprocess.run(cmd, cwd=repo_root)
            if result.returncode != 0:
                self.die(
                    f"`west build` failed for board '{board}' in '{build_dir}' with exit code {result.returncode}"
                )

    def _build_command(self, source_dir: Path, board: str, build_dir: str,
                       pristine: bool, debug: bool, passthrough_args):
        cmd = ["west", "build", str(source_dir), "-b", board, "-d", build_dir]
        if pristine:
            cmd.append("--pristine")

        cmake_args = []
        if debug:
            debug_conf = source_dir / "conf" / "debug.conf"
            if not debug_conf.exists():
                self.die(f"debug config does not exist: {debug_conf}")
            cmake_args.append(f"-DEXTRA_CONF_FILE={debug_conf}")

        if cmake_args or passthrough_args:
            cmd.append("--")
            cmd.extend(cmake_args)
            cmd.extend(passthrough_args)

        return board, build_dir, cmd

    def _resolve_build_targets(self, repo_root: Path, board: str,
                               build_dir: str | None):
        if "/" in board:
            return [(board, self._default_build_dir(board, build_dir))]

        board_dirs = sorted((repo_root / "boards").glob(f"*/{board}"))
        if not board_dirs:
            return [(board, build_dir or "build")]
        if len(board_dirs) > 1:
            self.die(f"board '{board}' is ambiguous; matching directories: " +
                     ", ".join(str(path) for path in board_dirs))

        board_dir = board_dirs[0]
        left_variants = []
        right_variants = set()

        for yaml_file in sorted(board_dir.glob(f"{board}_*.yaml")):
            qualifier = yaml_file.stem[len(board) + 1:].replace("_", "/")
            if qualifier.endswith("/left"):
                left_variants.append(qualifier)
            elif qualifier.endswith("/right"):
                right_variants.add(qualifier)

        split_pairs = [
            qualifier for qualifier in left_variants
            if qualifier[:-len("/left")] + "/right" in right_variants
        ]

        if len(split_pairs) == 1:
            base_qualifier = split_pairs[0][:-len("/left")]
            left_board = f"{board}/{base_qualifier}/left"
            right_board = f"{board}/{base_qualifier}/right"
            return [(left_board, "build-left"), (right_board, "build-right")]
        if len(split_pairs) > 1:
            self.die(
                f"board '{board}' has multiple split left variants; use a fully qualified target. "
                f"Candidates: {', '.join(f'{board}/{qualifier}' for qualifier in split_pairs)}"
            )

        return [(board, build_dir or "build")]

    def _default_build_dir(self, board: str, build_dir: str | None) -> str:
        if build_dir:
            return build_dir
        if board.endswith("/left"):
            return "build-left"
        if board.endswith("/right"):
            return "build-right"
        return "build"
