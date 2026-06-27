import argparse
import sys
import os
import subprocess
import time
import importlib

def main():
    parser = argparse.ArgumentParser(description="csilk CLI")
    subparsers = parser.add_subparsers(dest="command")
    
    run_parser = subparsers.add_parser("run", help="Run a csilk app with auto-reload")
    run_parser.add_argument("app", help="App to run, e.g., main:app")
    run_parser.add_argument("--port", type=int, default=8080, help="Port to bind")
    run_parser.add_argument("--reload", action="store_true", help="Enable auto-reload")
    
    args = parser.parse_args()
    
    if args.command == "run":
        if args.reload:
            if os.environ.get("CSILK_RELOAD_PROCESS") == "true":
                run_app(args.app, args.port)
            else:
                watch_and_reload(sys.argv)
        else:
            run_app(args.app, args.port)

def watch_and_reload(argv):
    print("Watching for file changes with stat poller...")
    env = os.environ.copy()
    env["CSILK_RELOAD_PROCESS"] = "true"
    
    process = subprocess.Popen(argv, env=env)
    
    mtimes = {}
    while True:
        try:
            changed = False
            for root, _, files in os.walk("."):
                for f in files:
                    if f.endswith(".py"):
                        path = os.path.join(root, f)
                        mtime = os.stat(path).st_mtime
                        if path not in mtimes:
                            mtimes[path] = mtime
                        elif mtimes[path] != mtime:
                            mtimes[path] = mtime
                            changed = True
                            
            if changed:
                print("Changes detected, reloading...")
                process.terminate()
                process.wait()
                process = subprocess.Popen(argv, env=env)
            
            time.sleep(1)
        except KeyboardInterrupt:
            process.terminate()
            sys.exit(0)

def run_app(app_str, port):
    module_name, app_name = app_str.split(":")
    sys.path.insert(0, os.getcwd())
    module = importlib.import_module(module_name)
    app = getattr(module, app_name)
    print(f"Starting {app_str} on port {port}...")
    app.run(port)

if __name__ == "__main__":
    main()
