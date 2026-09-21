# XXI

*The new default.*

**The most basic text editor with code running, undo-redo, and everything a working text editor needs.**

One file of C. No libraries. It just works.

---

## Get it

```sh
git clone https://github.com/SunqdXX/XXI.git
cd XXI
./install.sh
```

That is it. It builds itself and puts `xxi` where your terminal can find it.

**Even shorter** (one line, no cloning):

```sh
curl -fsSL https://raw.githubusercontent.com/SunqdXX/XXI/main/install.sh | sh
```

Want it for everyone on the computer?

```sh
sudo ./install.sh
```

---

## Use it

```sh
xxi notes.txt
```

If the file is not there, XXI makes a new one for you.

Now just type. That is it.

---

## The keys

| Press this | And it does this |
| --- | --- |
| `Ctrl-S` | Save |
| `Ctrl-W` | Quit. It asks first. Press `Enter` to really quit. |
| `Ctrl-C` | Copy |
| `Ctrl-V` | Paste |
| `Ctrl-K` | Delete the whole line |
| `Ctrl-R` | Undo |
| `Ctrl-Y` | Redo |
| `Ctrl-X` | Select stuff |
| `Ctrl-F` | Find |
| `Ctrl-T` | Run your code |
| `Alt-C` | Copy out of the terminal at the bottom |
| `Alt-V` | Paste into the terminal at the bottom |

You do not have to remember any of these. They are always listed on the right side of your screen.

---

## Run your code

1. Open a file, like `xxi hello.py`
2. Press `Ctrl-T`
3. It asks "run python3 hello.py?" — press `Enter`

Your code runs. The output shows up at the bottom. Press `Esc` to close it.

Press `Ctrl-T` again to run it again.

While your code runs, the keys go to it. `Ctrl-C` stops it — always, exactly like a normal terminal. And when you stop it you are left at a shell prompt in that same box, so you can just run something else. You never have to close the terminal to get out of a program.

Want a line of the output? **Just drag over it.** Letting go of the mouse copies it — no key at all. To paste something in, press `Alt-V`. Either way the terminal stays yours, you do not have to leave it to copy.

C and C++ get the math library linked for you, so `pow` and `sqrt` work without you having to know about `-lm`.

The box at the bottom is a real terminal, not a picture of one. Your colors come through, so a red compiler error stays red. Long lines wrap instead of falling off the edge. Programs that draw the whole screen work too — you can run `vim` down there if you like, and when you quit it your output comes back exactly as you left it.

One thing to know: `Esc` closes the terminal, **unless** a full-screen program is running — then `Esc` belongs to that program, the way it should. Press `Ctrl-T` to step back into the editor first.

It knows how to run **Python, C, C++, Rust, Go, JavaScript, Ruby, Lua, Perl, PHP, Java, R, Julia** and shell scripts. If it does not know your file, it gives you a normal terminal at the bottom instead.

---

## Select stuff

Press `Ctrl-X`. Now drag with your mouse, or use the arrow keys. What you picked lights up. Press `Backspace` and it is gone.

**Just want to copy something?** Do not press `Ctrl-X`. Just drag with your mouse and press `Ctrl-C`.

What you picked stays lit after you copy it. Press `Ctrl-C` ten times if you want — you get the same thing every time. Press `Esc`, click somewhere, or just start typing to let it go.

---

## Undo

Press `Ctrl-R`. Press it again. And again. It goes all the way back to when you opened the file.

Went back too far? `Ctrl-Y` goes forward again.

---

## Find

Press `Ctrl-F` and type. Every match lights up. Press `Enter` to jump to the next one. Press `Esc` when you are done.

---

## Good to know

- Click anywhere with your mouse to put the cursor there.
- Your clipboard is the real one. Copy in XXI, paste in your browser.
- If a file is read-only, XXI tells you and refuses to wreck it.
- No colors on your code. That is on purpose.
- `Alt-C` and `Alt-V` do the same as `Ctrl-Shift-C` and `Ctrl-Shift-V`, for terminals that keep those keys for themselves.

---

## Copying out of the terminal

Here is a thing about terminals that is worth knowing, because it looks like a bug and is
not one. **`Ctrl-Shift-C` never reaches the program you are running.** Your terminal —
kitty, GNOME Terminal, Konsole, Alacritty, foot, all of them — keeps that key for its own
copy and swallows it. And its copy works on *its* selection, which is empty here, because
XXI is reading the mouse itself. So in most terminals `Ctrl-Shift-C` quietly does nothing
in any program like this one.

So XXI does not rely on it:

- **Drag and let go.** That copies. No key, no setup, works in every terminal.
- **`Alt-C` / `Alt-V`** if you want a key. No terminal claims these.

If you would rather have `Ctrl-Shift-C` anyway, you have to tell your terminal to hand it
over. XXI titles its window `XXI: <file>`, so you can aim the rule at XXI alone and leave
copy and paste alone everywhere else. In kitty that is two lines in `kitty.conf`:

```conf
map --when-focus-on title:^XXI: ctrl+shift+c send_text all \x1b[99;6u
map --when-focus-on title:^XXI: ctrl+shift+v send_text all \x1b[118;6u
```

Reload with `Ctrl-Shift-F5` and restart XXI. Other terminals have their own way of saying
the same thing; the escape codes to send are the kitty keyboard protocol's, which XXI asks
for on startup.

---

## That is everything

Really. There is nothing else to learn.
