"""Command-line interface for csilk.

Provides the ``csilk`` CLI entry point with subcommands::

    csilk run main:app --port 8080 --reload
    csilk run-wf workflow.yaml --port 3000
"""

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
    run_parser.add_argument("--asgi", action="store_true", help="Run application as an ASGI app")
    
    wf_parser = subparsers.add_parser("run-wf", help="Run a csilk workflow from a YAML or JSON file")
    wf_parser.add_argument("file", help="Path to the .yaml, .yml, or .json workflow definition")
    wf_parser.add_argument("--input", default="", help="Initial input string to the workflow")
    wf_parser.add_argument("--port", type=int, default=0, help="Port to bind for the UI dashboard (0 = disable)")
    
    args = parser.parse_args()
    
    if args.command == "run":
        if args.reload:
            if os.environ.get("CSILK_RELOAD_PROCESS") == "true":
                run_app(args.app, args.port, asgi=args.asgi)
            else:
                watch_and_reload(sys.argv)
        else:
            run_app(args.app, args.port, asgi=args.asgi)
            
    elif args.command == "run-wf":
        run_workflow(args.file, args.input, args.port)

def run_workflow(file_path, initial_input, port):
    """Load a workflow from a YAML or JSON file and run it, optionally serving a UI."""
    from csilk.workflow import Workflow
    from csilk.app import App
    import threading
    import time
    
    if not os.path.exists(file_path):
        print(f"Error: Workflow file '{file_path}' not found.")
        sys.exit(1)
        
    print(f"Loading workflow from {file_path}...")
    if file_path.endswith(".yaml") or file_path.endswith(".yml"):
        wf = Workflow.load_yaml(file_path)
    elif file_path.endswith(".json"):
        with open(file_path, "r") as f:
            wf = Workflow.from_json(f.read())
    else:
        print("Error: Unsupported file format. Use .yaml or .json")
        sys.exit(1)
        
    if not wf:
        print("Error: Failed to parse workflow.")
        sys.exit(1)
        
    app = None
    if port > 0:
        app = App()
        Workflow.serve_ui(app, "/workflow/ui")
        def start_ui():
            print(f"Workflow UI available at http://localhost:{port}/workflow/ui")
            app.run(port)
        ui_thread = threading.Thread(target=start_ui, daemon=True)
        ui_thread.start()
        time.sleep(0.5)
        
    print(f"Running workflow '{wf._wf}' with input: '{initial_input}'")
    
    event = threading.Event()
    result_data = []
    
    def on_complete(res):
        if res:
            result_data.append(res)
        event.set()
        
    wf.run(initial_input, callback=on_complete)
    event.wait()
    
    print("\n=== Workflow Completed ===")
    if result_data:
        print(f"Result: {result_data[0]}")
    else:
        print("No result returned.")
        
    if app:
        app.stop()

def watch_and_reload(argv):
    """Poll ``.py`` files for modifications and restart the server on change."""
    print("Watching for file changes with stat poller...")
    env = os.environ.copy()
    env["CSILK_RELOAD_PROCESS"] = "true"
    
    argv_to_run = [sys.executable, "-m", "csilk.cli"] + argv[1:]
    
    process = subprocess.Popen(argv_to_run, env=env)
    
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
                process = subprocess.Popen(argv_to_run, env=env)
            
            time.sleep(1)
        except KeyboardInterrupt:
            process.terminate()
            sys.exit(0)

def run_app(app_str, port, asgi=False):
    """Import and run a csilk or ASGI application by ``module:attr`` name."""
    module_name, app_name = app_str.split(":")
    sys.path.insert(0, os.getcwd())
    module = importlib.import_module(module_name)
    app = getattr(module, app_name)
    
    if asgi:
        from csilk.app import App
        app = App(asgi_app=app)
        
    print(f"Starting {app_str} on port {port}...")
    app.run(port)

if __name__ == "__main__":
    main()
