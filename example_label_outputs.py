#!/usr/bin/env python3

# This script, when run under swaylock-plugin --command-each,
# will display the name of each output in the background

import subprocess
import os
import tempfile

text = os.getenv("SWAYLOCK_PLUGIN_OUTPUT_NAME", default="Use 'each'")
text = text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
template = """<?xml version="1.0" encoding="UTF-8" standalone="no"?>
<svg width="200mm" height="200mm" viewBox="0 0 200 200" version="1.1"
    xmlns="http://www.w3.org/2000/svg" xmlns:svg="http://www.w3.org/2000/svg">
  <text x="100" y="100" fill="red" text-anchor="middle" font-size="50px">{}</text>
</svg>
"""
with tempfile.NamedTemporaryFile(suffix=".svg") as f:
    f.write(template.format(text).encode())
    f.flush()

    command = ["swaybg", "-m", "fit", "-i", f.name]
    subprocess.run(command, close_fds=False)
