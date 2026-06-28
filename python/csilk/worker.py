import json
import traceback

class WorkflowWorker:
    """A distributed worker that consumes workflow tasks from MQ, executes them,
    and publishes the results back to the workflow engine.
    """
    
    def __init__(self, mq, topic_tasks="csilk.wf.tasks", topic_results="csilk.wf.results"):
        self.mq = mq
        self.topic_tasks = topic_tasks
        self.topic_results = topic_results
        self.handlers = {}
        
    def register(self, node_id, handler_fn):
        """Register a handler function for a specific remote node_id.
        
        Args:
            node_id (str): The node ID this worker handles.
            handler_fn (callable): A function `(input_data: str) -> str`
        """
        self.handlers[node_id] = handler_fn
        
    def start(self):
        """Start listening for tasks on the message queue."""
        @self.mq.subscribe(self.topic_tasks)
        def _process_task(ctx):
            try:
                task = json.loads(ctx.payload)
                exec_id = task.get("exec_id")
                node_id = task.get("node_id")
                input_data = task.get("input", "")
                
                if node_id in self.handlers:
                    try:
                        output_data = self.handlers[node_id](input_data)
                    except Exception as e:
                        print(f"[Worker] Error executing node {node_id}: {e}")
                        traceback.print_exc()
                        output_data = f"Error: {str(e)}"
                        
                    result = {
                        "exec_id": exec_id,
                        "node_id": node_id,
                        "output": str(output_data) if output_data is not None else ""
                    }
                    self.mq.publish(self.topic_results, json.dumps(result))
            except Exception as e:
                print(f"[Worker] Failed to parse or process task: {e}")
