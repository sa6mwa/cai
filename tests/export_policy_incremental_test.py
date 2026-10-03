"""Prove export changes relink without explicit configure or source edits."""
import pathlib
import subprocess
import sys
import tempfile


def main():
    repo, build_root = map(pathlib.Path, sys.argv[1:3])
    cc, nm, cmake = sys.argv[3:6]
    with tempfile.TemporaryDirectory(dir=build_root, prefix="export-incremental-") as temp:
        source = pathlib.Path(temp)
        build = source / "build"
        manifest = source / "symbols"
        manifest.write_text("one\n")
        (source / "sample.c").write_text(
            "int one(void); int two(void);\n"
            "int one(void) { return 1; }\nint two(void) { return 2; }\n")
        (source / "CMakeLists.txt").write_text(
            'cmake_minimum_required(VERSION 3.21)\nproject(Exports C)\n'
            f'include("{repo / "cmake/cai-export-policy.cmake"}")\n'
            'add_library(sample SHARED sample.c)\n'
            'target_compile_options(sample PRIVATE -Wall -Wextra -Werror)\n'
            'target_link_options(sample PRIVATE -Wl,--fatal-warnings)\n'
            'cai_apply_export_policy(sample "${CMAKE_CURRENT_SOURCE_DIR}/symbols")\n')
        configure = [cmake, "-S", str(source), "-B", str(build), "-G", "Ninja",
                     f"-DCMAKE_C_COMPILER={cc}"]
        command = [cmake, "--build", str(build), "--target", "sample"]

        def run(args):
            result = subprocess.run(args, capture_output=True, text=True, timeout=10)
            assert result.returncode == 0, result.stdout + result.stderr

        def exports():
            result = subprocess.check_output(
                [nm, "-D", "--defined-only", str(build / "libsample.so")], text=True)
            return sorted(line.split()[-1] for line in result.splitlines())

        run(configure)
        run(command)
        assert exports() == ["one"]
        library = build / "libsample.so"
        linker_map = build / "sample.export-map"
        times = (library.stat().st_mtime_ns, linker_map.stat().st_mtime_ns)
        run(configure)
        run(command)
        assert times == (library.stat().st_mtime_ns, linker_map.stat().st_mtime_ns), \
            "unchanged configure must preserve the map and avoid relinking"
        manifest.write_text("one\ntwo\n")
        run(command)  # No explicit configure, object rebuild, or clean.
        assert exports() == ["one", "two"], "new exports must reach the library"
        assert library.stat().st_mtime_ns != times[0]
        manifest.write_text("two\n")
        run(command)
        assert exports() == ["two"], "removed exports must become private"
        stamp = library.stat().st_mtime_ns
        manifest.write_text("two\ntwo\n")
        failed = subprocess.run(command, capture_output=True, text=True, timeout=10)
        assert failed.returncode != 0 and "duplicate symbol" in failed.stdout + failed.stderr
        assert library.stat().st_mtime_ns == stamp, "invalid policy must not republish"
    print("incremental export additions, removals, unchanged rebuilds and invalid policy passed")


if __name__ == "__main__":
    main()
