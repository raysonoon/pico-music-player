# Revision Log

Date: 2026-09-23


---
#### Create a Python .bat file for non-technical setup

- `setup.bat` — double-click to open Windows launcher that walks the user through Wi-Fi setup, program installation and initialisation for Pico over USB
- print a prominent warning banner before any prompts: the **Pico W
  only supports 2.4 GHz** Wi-Fi and cannot see 5 GHz networks

---
####  Shift whole display right

- compensate for a left-edge hardware defect on the OLED by shifting the whole frame right by 8 pixels at display time.

---
#### Find song mode: add a cap on number of times button is pressed

- cap the captured taps at 16 in find a song mode; extra taps beyond the cap are ignored.
- fixed a stray digit in the code that broke the limit on the captured taps

--- 
#### Refine UI text layout

- wrap long single-line messages into multiple lines for readability, e.g. break "craving connection" across two lines instead of one long line.