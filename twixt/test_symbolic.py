#!/usr/bin/env python3
"""Integration checks for exact symbolic solving and portable artifacts."""

from pathlib import Path
import subprocess
import tempfile


def run(*args, code=0):
    result = subprocess.run(["./twixt", *map(str, args)], text=True, capture_output=True, timeout=180)
    if result.returncode != code:
        raise AssertionError((args, result.returncode, result.stdout, result.stderr))
    return result.stdout + result.stderr


with tempfile.TemporaryDirectory(prefix="twixt-dfa-test-") as directory:
    path = Path(directory) / "solution.twixtdfa"
    for board, expected in [("3x3", "draw"), ("4x4", "draw"), ("3x5", "black"),
                            ("5x3", "white"), ("4x5", "black")]:
        output = run("--board", board, "--symbolic", "--symbolic-verify",
                     "--census-seconds", 0, "--expect", expected, "--dfa-out", path)
        assert "WDL values against explicit retrograde" in output
        probe = run("--probe-dfa", path, "--quiet")
        assert "DFA WDL" in probe and "Moves:" in probe
        if board == "4x5":
            assert "Verified all 304210 WDL values" in output
            assert "to move): loss" in probe
            child = run("--probe-dfa", path, "--moves", "B1", "--quiet")
            assert "to move): win" in child

    # A structurally corrupt or truncated artifact must fail deterministically.
    original = path.read_bytes()
    for bad in (b"not a DFA", original[:-1], original + b"extra"):
        path.write_bytes(bad)
        run("--probe-dfa", path, code=1)
    path.write_bytes(original)

    reference = Path(directory) / "reference.twixtdfa"
    reference.write_bytes(original)
    for backend, workers in [("threads", 8), ("processes", 4)]:
        output = run("--board", "4x5", "--symbolic", "--symbolic-verify",
                     "--workers", workers, "--parallel-backend", backend,
                     "--parallel-min-nodes", 0, "--parallel-temp-dir", directory,
                     "--compare-dfa", reference, "--census-seconds", 0, "--dfa-out", path)
        assert "Exact W/L/D language equivalence passed" in output
        assert "Verified all 304210 WDL values" in output
        assert not list(Path(directory).glob("twixt-workers-*"))
    output = run("--board", "3x5", "--symbolic", "--workers", 64,
                 "--parallel-min-nodes", 0, "--symbolic-verify", "--census-seconds", 0)
    assert "Verified all 1155 WDL values" in output
    run("--probe-dfa", path, "--compare-dfa", reference, "--quiet")
    corrupted_values = bytearray(original)
    corrupted_values[84:88], corrupted_values[88:92] = corrupted_values[88:92], corrupted_values[84:88]
    wrong_values = Path(directory) / "wrong-values.twixtdfa"
    wrong_values.write_bytes(corrupted_values)
    run("--probe-dfa", wrong_values, "--compare-dfa", reference, "--quiet", code=1)
    run("--symbolic", "--workers", 0, code=1)
    run("--symbolic", "--workers", 65, code=1)
    run("--symbolic", "--parallel-backend", "invalid", code=1)
    run("--strong", "--workers", 2, code=1)
    run("--compare-dfa", reference, code=1)

    capped = Path(directory) / "capped.twixtdfa"
    run("--size", 5, "--symbolic", "--dfa-max-nodes", 64,
        "--census-seconds", 0, "--dfa-out", capped, code=3)
    assert not capped.exists()
    for backend in ["threads", "processes"]:
        output = run("--size", 6, "--symbolic", "--workers", 4,
                     "--parallel-backend", backend, "--parallel-min-nodes", 0,
                     "--parallel-temp-dir", directory, "--census-seconds", 0.05,
                     "--dfa-out", capped, "--dfa-checkpoint", Path(directory) / "cap.twixtckp", code=3)
        assert not capped.exists()
        assert not list(Path(directory).glob("twixt-workers-*"))
    run("--symbolic-ply", 3, "--dfa-out", capped, code=1)
    run("--symbolic-verify", code=1)
    run("--symbolic", "--strong", code=1)

    checkpoint = Path(directory) / "resume.twixtckp"
    output = run("--board", "4x5", "--symbolic-ply", 4,
                 "--dfa-checkpoint", checkpoint, "--census-seconds", 0)
    assert "no WDL solution claimed" in output
    output = run("--board", "4x5", "--symbolic-ply", 4, "--workers", 64,
                 "--parallel-min-nodes", 0, "--compare-dfa", checkpoint, "--census-seconds", 0)
    assert "Exact reachable/terminal/active language equivalence passed" in output
    run("--probe-dfa", checkpoint, code=1)
    resumed = run("--resume-dfa", checkpoint, "--symbolic-verify", "--workers", 8,
                  "--parallel-min-nodes", 0,
                  "--dfa-checkpoint", checkpoint, "--census-seconds", 0,
                  "--dfa-out", path)
    assert "Verified all 304210 WDL values" in resumed
    # A completed checkpoint resumes without recomputing the solution.
    output = run("--resume-dfa", checkpoint, "--census-seconds", 0, "--dfa-out", path)
    assert "Symbolic strong result: black" in output
    assert "Symbolic forward generation" not in output
    assert not Path(str(checkpoint) + ".partial").exists()

    # Interrupt after at least one retrograde layer has been checkpointed;
    # resumption must preserve already solved values and finish the prefix.
    process = subprocess.Popen(
        ["./twixt", "--board", "4x5", "--symbolic", "--dfa-checkpoint", str(checkpoint),
         "--census-seconds", "0"], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    in_retrograde = False
    for line in process.stdout:
        if line.startswith("Symbolic retrograde"):
            in_retrograde = True
        if in_retrograde and line.startswith("15,"):
            process.terminate()
            process.wait(timeout=10)
            break
    else:
        raise AssertionError("retrograde checkpoint was not observed")
    resumed = run("--resume-dfa", checkpoint, "--symbolic-verify", "--census-seconds", 0,
                  "--dfa-out", path)
    assert "Verified all 304210 WDL values" in resumed

    # A capped explicit census must not label a partial count as complete.
    row = run("--board", "5x6", "--census-ply", 3,
              "--census-max-states", 2, "--census-seconds", 0, code=3)
    assert ",NA,NA," in row

print("symbolic integration: exhaustive 3x3, 4x4, 3x5, 5x3, 4x5 checks passed")
