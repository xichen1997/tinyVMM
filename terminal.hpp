
#pragma once
 
#include <termios.h>
#include <unistd.h>
 
// set stdin to raw mode, restore when destructed. do nothing if stdin is not a terminal (e.g. redirected from a file).
class RawTerminal {
public:
    RawTerminal() {
        if (!isatty(STDIN_FILENO) || tcgetattr(STDIN_FILENO, &old_) < 0)
            return;
        termios raw = old_;
        raw.c_lflag &= ~(ICANON | ECHO | ISIG);   // no newline, no local echo, Ctrl-C is passed to guest
        raw.c_iflag &= ~(ICRNL | IXON);           // carriage return is passed as is, Ctrl-S/Ctrl-Q is also passed to guest
        raw.c_cc[VMIN] = 1;                       // read at least 1 byte
        raw.c_cc[VTIME] = 0;
        // c_oflag is unchanged: output still converts \n to \r\n, so guest's output will not be misaligned
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        active_ = true;
    }
 
    ~RawTerminal() { restore(); }
 
    void restore() {
        if (active_)
            tcsetattr(STDIN_FILENO, TCSANOW, &old_);
        active_ = false;
    }
 
    RawTerminal(const RawTerminal&) = delete;
    RawTerminal& operator=(const RawTerminal&) = delete;
 
private:
    termios old_{};
    bool active_ = false;
};
 