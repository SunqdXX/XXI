# XXI

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

You do not have to remember any of these. They are always listed on the right side of your screen.

---

## Run your code

1. Open a file, like `xxi hello.py`
2. Press `Ctrl-T`
3. It asks "run python3 hello.py?" — press `Enter`

Your code runs. The output shows up at the bottom. Press `Esc` to close it.

Press `Ctrl-T` again to run it again.

It knows how to run **Python, C, C++, Rust, Go, JavaScript, Ruby, Lua, Perl, PHP, Java, R, Julia** and shell scripts. If it does not know your file, it gives you a normal terminal at the bottom instead.

---

## Select stuff

Press `Ctrl-X`. Now drag with your mouse, or use the arrow keys. What you picked lights up. Press `Backspace` and it is gone.

**Just want to copy something?** Do not press `Ctrl-X`. Just drag with your mouse and press `Ctrl-C`.

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

---

## That is everything

Really. There is nothing else to learn.
