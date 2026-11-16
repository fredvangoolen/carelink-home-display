# Double-click on Windows: starts the monitor without a console window.
# The program itself is minimed_desktop.py, next to this file.
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import minimed_desktop
minimed_desktop.main()
