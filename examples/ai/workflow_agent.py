import sys
import os
import time

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', 'python')))
from csilk.workflow import Workflow, WorkflowContext, WorkflowData
from csilk.app import App
import threading

def main():
    print("🚀 Initializing csilk AI Workflow Agent...")
    
    # The workflow engine relies on libuv default loop
    app = App()
    def start_loop():
        app.run(8083)  # Dummy port
    loop_thread = threading.Thread(target=start_loop, daemon=True)
    loop_thread.start()
    time.sleep(0.5)

    wf = Workflow("stock_analyzer")

    # 1. Define nodes
    def query_input(ctx: WorkflowContext, inp: WorkflowData):
        val = inp.value if inp.value else "Apple"
        print(f"📥 Received input: {val}")
        return ctx.data_new_string(f"Please provide an analysis for the stock: {val}")

    def format_output(ctx: WorkflowContext, inp: WorkflowData):
        val = inp.value
        if not val:
            val = "Error: No output from AI node (possibly missing OPENAI_API_KEY)"
        print("✅ Analysis Complete!")
        print("-" * 40)
        print(val)
        print("-" * 40)
        return ctx.data_new_string(val)

    # 2. Add python handlers
    n_input = wf.add("query_input", query_input)
    n_input.set_entry(True)

    n_output = wf.add("format_output", format_output)

    # 3. Add AI Agent node
    n_ai = wf.add_ai(
        node_id="advisor_ai",
        model="gpt-3.5-turbo",
        system_msg="You are a professional financial advisor. You give concise and sharp analysis.",
        prompt="{{node.value}}"
    )

    # 4. Register a tool for the AI to call
    def fetch_stock_price(args_json: str):
        import json
        try:
            args = json.loads(args_json)
            ticker = args.get("ticker", "AAPL")
            print(f"🛠️  Tool called: Fetching real-time price for {ticker}...")
            # Mock data
            price = 150.0 if ticker == "AAPL" else 300.0
            return {"ticker": ticker, "price": price, "currency": "USD"}
        except Exception as e:
            return {"error": str(e)}

    wf.register_tool(
        name="get_stock_price",
        description="Fetch the current stock price for a given ticker symbol.",
        parameters_json='{"type": "object", "properties": {"ticker": {"type": "string"}}, "required": ["ticker"]}',
        tool_fn=fetch_stock_price
    )

    # 5. Connect nodes in a graph
    n_input.bind(n_ai)
    n_ai.bind(n_output)

    # 6. Run workflow asynchronously
    print("🔄 Running workflow...")
    
    # We use a mutable list to store the result from the async callback
    result_box = []
    def on_complete(res):
        if res:
            result_box.append(res.value)
        else:
            result_box.append(None)
            
    exec_id = wf.run("AAPL", callback=on_complete)
    print(f"🆔 Execution ID: {exec_id}")
    
    # Wait for completion (since it runs on libuv loop in background thread)
    while not result_box:
        time.sleep(0.1)
        
    print("👋 Shutting down...")
    app.stop()
    loop_thread.join(timeout=2.0)

if __name__ == "__main__":
    main()
