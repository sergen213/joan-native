#!/usr/bin/env python3
"""Run one GitHub Actions job of .github/workflows/*.yml locally in its archlinux container (no `act`).

  run_workflow_local.py WORKFLOW JOB [--source DIR] [--cache DIR] [--signing-key FILE] [--tag v0.1.0]

All `run:` steps execute in ONE container (like Actions), with GITHUB_ENV/GITHUB_WORKSPACE/RUNNER_TEMP
emulated. `uses:` steps: actions/checkout copies --source into a throwaway git repo inside the container
(tagged --tag for release.yml; never touches the host repository), actions/cache bind-mounts --cache
at its `path`. Secrets: JOA_UPDATE_SIGNING_KEY from --signing-key (a DEV key; never the production key).
Steps that publish (gh release) are printed, not executed. Nothing leaves this machine.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import uuid
from pathlib import Path

import yaml

WORKSPACE = "/__w/joan-native/joan-native"
PUBLISH = re.compile(r"\bgh release (create|upload|edit)\b|\bgit push\b")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("workflow", type=Path)
    parser.add_argument("job")
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--cache", type=Path, help="host dir for actions/cache paths (default: fresh)")
    parser.add_argument("--signing-key", type=Path)
    parser.add_argument("--tag", default="v0.1.0")
    parser.add_argument("--epoch", default="1791953501", help="commit time of the throwaway checkout")
    args = parser.parse_args()

    workflow = yaml.safe_load(args.workflow.read_text())
    job = workflow["jobs"][args.job]
    container = job["container"]
    image = container if isinstance(container, str) else container["image"]
    options = [] if isinstance(container, str) else container.get("options", "").split()
    secrets = {"JOA_UPDATE_SIGNING_KEY": args.signing_key.read_text().strip() if args.signing_key else ""}
    name = f"joa-wf-{args.job}-{uuid.uuid4().hex[:8]}"
    mounts = ["-v", f"{args.source.resolve()}:/source:ro"]
    for step in job["steps"]:
        if step.get("uses", "").startswith("actions/cache"):
            cache = (args.cache or Path(f"/var/tmp/{name}-cache")).resolve()
            cache.mkdir(parents=True, exist_ok=True)
            mounts += ["-v", f"{cache}:{WORKSPACE}/{step['with']['path']}"]
    subprocess.run(["docker", "run", "-d", "--name", name, *options, *mounts, image, "sleep", "infinity"],
                   check=True, stdout=subprocess.DEVNULL)
    env = {"CI": "true", "GITHUB_ACTIONS": "true", "GITHUB_WORKSPACE": WORKSPACE, "RUNNER_TEMP": "/__w/_temp",
           "GITHUB_ENV": "/__w/_temp/github_env", "GITHUB_REPOSITORY": "OWNER/joan-native",
           "GITHUB_REF_NAME": args.tag, "GITHUB_REF": f"refs/tags/{args.tag}", "HOME": "/github/home"}

    def expand(text: str) -> str:
        text = re.sub(r"\$\{\{\s*secrets\.(\w+)\s*\}\}", lambda m: secrets.get(m.group(1), ""), text)
        text = re.sub(r"\$\{\{\s*github\.repository\s*\}\}", env["GITHUB_REPOSITORY"], text)
        text = re.sub(r"\$\{\{\s*github\.token\s*\}\}", "local-dry-run", text)
        return re.sub(r"\$\{\{\s*github\.ref\s*\}\}", env["GITHUB_REF"], text)

    def sh(script: str, step_env: dict, cwd: str = WORKSPACE) -> int:
        command = ["docker", "exec", "-w", cwd]
        for key, value in {**env, **step_env}.items():
            command += ["-e", f"{key}={value}"]
        return subprocess.run(command + [name, "bash", "-eo", "pipefail", "-c", script]).returncode

    try:
        sh(f"mkdir -p {WORKSPACE} /__w/_temp /github/home && touch $GITHUB_ENV", {}, cwd="/")
        for index, step in enumerate(job["steps"], 1):
            title = step.get("name") or step.get("uses") or step["run"].splitlines()[0]
            print(f"\n=== [{args.job} {index}/{len(job['steps'])}] {title}", flush=True)
            uses = step.get("uses", "")
            if uses.startswith("actions/checkout"):
                code = sh(f"git config --global --add safe.directory {WORKSPACE} && tar -C /source --exclude=./work -cf - . | tar --no-same-owner -xf - && git init -q && git add -A && "
                          f"GIT_AUTHOR_DATE=@{args.epoch} GIT_COMMITTER_DATE=@{args.epoch} git -c user.name=ci "
                          f"-c user.email=ci@localhost commit -qm local && git tag {args.tag}", {})
            elif uses.startswith("actions/cache"):
                print(f"(cache bind-mounted at {step['with']['path']})")
                continue
            elif uses:
                sys.exit(f"unsupported action {uses}")
            elif PUBLISH.search(step["run"]):
                print("DRY RUN (publishing is never done locally):\n" + expand(step["run"]))
                code = sh("ls -la \"${DIST:-.}\"", {})
            else:
                step_env = {k: expand(str(v)) for k, v in step.get("env", {}).items()}
                exported = subprocess.run(["docker", "exec", name, "cat", "/__w/_temp/github_env"],
                                          capture_output=True, text=True).stdout
                env.update(line.split("=", 1) for line in exported.splitlines() if "=" in line)
                code = sh(expand(step["run"]), step_env)
            if code:
                sys.exit(f"step failed ({code}): {title}")
        print(f"\nJOB {args.job} PASSED ({len(job['steps'])} steps)")
    finally:
        subprocess.run(["docker", "rm", "-f", name], stdout=subprocess.DEVNULL)


if __name__ == "__main__":
    main()
