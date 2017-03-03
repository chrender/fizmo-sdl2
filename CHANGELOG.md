


   **Version 0.8.4**

 - Raised version number to 0.8.4. It should have been in the 0.8 range from the beginning, since this is the fizmo distribution version which releases the SDL2 interface. The second reasons for the jump from 0.7.1 to 0.8.4 is that all modules and frontends have so far gotten their version number from the distribution version they were split from, so fizmo-sdl2 now follows in this pattern. The new version number should also avoid some confusion for all packaging which is based on the modules instead of the whole distribution.
 - Fixed SDL2-event evaluation on startup. This fixes a bug that made the interpreter crash when your initial screen size was less than the default size. This fix does now also allow resizing the window during frontispiece display.
 - Adapted to split-library build.
 - Adapted to replacement of en_US locale with en_GB from libfizmo.
 - Show warning messages if window width or height supplied from the command line are too small.
 - Fix startup error messages, this also fixes silent exists in case the story file could not be found.
 - Fixed missing output of interpreter-related, fatal in-game-occuring error messages.

---


   **Version 0.7.1 — August 25, 2016**

 - Use tiny-xml-doc-tools for documentation.

---


   **Version 0.7.0 — July 28, 2016**

 - First implementation.


