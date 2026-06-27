from csilk.app import App

app = App()

@app.get("/")
def home(ctx):
    ctx.string(200, "Hello World from Hot Reload! Version 2")
