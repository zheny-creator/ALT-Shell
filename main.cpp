#include <array>
#include <chrono>
#include <cctype>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits.h>
#include <map>
#include <optional>
#include <poll.h>
#include <termios.h>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <vector>

extern char** environ;
namespace fs = std::filesystem;

namespace {
constexpr const char* VERSION = "0.2.0";

struct Token {
    enum class Type { Word, Pipe, In, Out, Append, ErrOut, Semi, Amp } type;
    std::string value;
};

struct Command {
    std::vector<std::string> argv;
    std::string input;
    std::string output;
    std::string error;
    bool append = false;
};

struct Job {
    std::vector<Command> commands;
    bool background = false;
};

std::string homeDir() {
    const char* h = std::getenv("HOME");
    if (h && *h) return h;
    passwd* pw = getpwuid(getuid());
    return pw && pw->pw_dir ? pw->pw_dir : "/";
}

fs::path configDir() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "alt-shell";
    return fs::path(homeDir()) / ".config" / "alt-shell";
}

fs::path dataDir() {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    if (xdg && *xdg) return fs::path(xdg) / "alt-shell";
    return fs::path(homeDir()) / ".local" / "share" / "alt-shell";
}


std::string trim(const std::string& s);

std::vector<std::string> splitCsv(const std::string& value) {
    std::vector<std::string> out;
    std::istringstream in(value);
    std::string item;
    while (std::getline(in, item, ',')) {
        item = trim(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

bool parseBool(const std::string& value, bool fallback) {
    std::string v = value;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    return fallback;
}

std::string expandConfigPath(std::string value) {
    if (value == "~") return homeDir();
    if (value.rfind("~/", 0) == 0) return (fs::path(homeDir()) / value.substr(2)).string();
    return value;
}

struct ShellConfig {
    bool containerEnabled = true;
    bool containerNetwork = false;
    bool workspaceWritable = true;
    std::vector<std::string> readonlyPaths = {"/usr", "/etc", "/bin", "/sbin", "/lib", "/lib64"};
    std::vector<std::string> extraReadonlyPaths;
    std::vector<std::string> extraWritablePaths;
    std::vector<std::string> allowedCommands;
    std::vector<std::string> passEnvironment = {"TERM", "LANG", "LC_ALL", "USER", "LOGNAME", "EDITOR"};
};

size_t previousUtf8Boundary(const std::string& value, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(value[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

size_t nextUtf8Boundary(const std::string& value, size_t pos) {
    if (pos >= value.size()) return value.size();
    ++pos;
    while (pos < value.size() && (static_cast<unsigned char>(value[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

size_t utf8Columns(const std::string& value, size_t end) {
    end = std::min(end, value.size());
    size_t columns = 0;
    for (size_t i = 0; i < end; ++i) {
        if ((static_cast<unsigned char>(value[i]) & 0xC0) != 0x80) ++columns;
    }
    return columns;
}


std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

bool validName(const std::string& s) {
    if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_';
    });
}

std::string hexEncode(const std::string& s) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out.push_back(digits[c >> 4]);
        out.push_back(digits[c & 0x0f]);
    }
    return out;
}

bool hexDecode(const std::string& s, std::string& out) {
    if (s.size() % 2) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = digit(s[i]), b = digit(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(static_cast<char>((a << 4) | b));
    }
    return true;
}

std::string expandVariable(const std::string& name, int lastStatus) {
    if (name == "?") return std::to_string(lastStatus);
    if (name == "$") return std::to_string(getpid());
    const char* value = std::getenv(name.c_str());
    return value ? value : "";
}

bool lexLine(const std::string& line, int lastStatus, std::vector<Token>& tokens, std::string& error) {
    tokens.clear();
    std::string word;
    bool started = false;
    enum class Quote { None, Single, Double } quote = Quote::None;

    auto pushWord = [&]() {
        if (started) {
            tokens.push_back({Token::Type::Word, word});
            word.clear();
            started = false;
        }
    };

    for (size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (quote == Quote::Single) {
            if (c == '\'') quote = Quote::None;
            else word += c;
            continue;
        }
        if (quote == Quote::Double) {
            if (c == '"') { quote = Quote::None; continue; }
            if (c == '\\' && i + 1 < line.size() && (line[i + 1] == '"' || line[i + 1] == '\\' || line[i + 1] == '$')) {
                word += line[++i]; continue;
            }
            if (c == '$') {
                std::string name;
                if (i + 1 < line.size() && line[i + 1] == '{') {
                    size_t end = line.find('}', i + 2);
                    if (end == std::string::npos) { error = "missing '}' in variable expansion"; return false; }
                    name = line.substr(i + 2, end - (i + 2)); i = end;
                } else if (i + 1 < line.size() && (std::isalpha(static_cast<unsigned char>(line[i + 1])) || line[i + 1] == '_' || line[i + 1] == '?' || line[i + 1] == '$')) {
                    size_t j = i + 1;
                    if (line[j] == '?' || line[j] == '$') name += line[j++];
                    else while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) name += line[j++];
                    i = j - 1;
                } else { word += '$'; continue; }
                word += expandVariable(name, lastStatus); started = true; continue;
            }
            word += c;
            continue;
        }

        if (c == '\'') { quote = Quote::Single; started = true; continue; }
        if (c == '"') { quote = Quote::Double; started = true; continue; }
        if (c == '\\') {
            if (i + 1 >= line.size()) { error = "trailing escape character"; return false; }
            word += line[++i]; started = true; continue;
        }
        if (c == '$') {
            std::string name;
            if (i + 1 < line.size() && line[i + 1] == '{') {
                size_t end = line.find('}', i + 2);
                if (end == std::string::npos) { error = "missing '}' in variable expansion"; return false; }
                name = line.substr(i + 2, end - (i + 2)); i = end;
            } else if (i + 1 < line.size() && (std::isalpha(static_cast<unsigned char>(line[i + 1])) || line[i + 1] == '_' || line[i + 1] == '?' || line[i + 1] == '$')) {
                size_t j = i + 1;
                if (line[j] == '?' || line[j] == '$') name += line[j++];
                else while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_')) name += line[j++];
                i = j - 1;
            } else { word += '$'; started = true; continue; }
            word += expandVariable(name, lastStatus); started = true; continue;
        }
        if (c == '~' && !started && (i + 1 == line.size() || line[i + 1] == '/' || line[i + 1] == ' ' || line[i + 1] == '\t')) {
            word += homeDir(); started = true; continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') { pushWord(); continue; }

        auto op = [&](Token::Type type) { pushWord(); tokens.push_back({type, {}}); };
        if (c == '|') { op(Token::Type::Pipe); continue; }
        if (c == ';') { op(Token::Type::Semi); continue; }
        if (c == '&') { op(Token::Type::Amp); continue; }
        if (c == '<') { op(Token::Type::In); continue; }
        if (c == '2' && word.empty() && !started && i + 1 < line.size() && line[i + 1] == '>') {
            tokens.push_back({Token::Type::ErrOut, {}});
            ++i;
            continue;
        }
        if (c == '>') {
            pushWord();
            if (i + 1 < line.size() && line[i + 1] == '>') { tokens.push_back({Token::Type::Append, {}}); ++i; }
            else tokens.push_back({Token::Type::Out, {}});
            continue;
        }
        word += c; started = true;
    }
    if (quote != Quote::None) { error = "unclosed quote"; return false; }
    pushWord();
    return true;
}

bool parseCommand(const std::vector<Token>& tokens, Command& cmd, std::string& error) {
    cmd = Command{};
    for (size_t i = 0; i < tokens.size(); ++i) {
        const auto type = tokens[i].type;
        if (type == Token::Type::In || type == Token::Type::Out || type == Token::Type::Append || type == Token::Type::ErrOut) {
            if (i + 1 >= tokens.size() || tokens[i + 1].type != Token::Type::Word) {
                error = "redirection requires a file name"; return false;
            }
            const std::string file = tokens[++i].value;
            if (type == Token::Type::In) cmd.input = file;
            else if (type == Token::Type::ErrOut) cmd.error = file;
            else { cmd.output = file; cmd.append = type == Token::Type::Append; }
        } else if (type == Token::Type::Word) cmd.argv.push_back(tokens[i].value);
        else { error = "unexpected operator"; return false; }
    }
    if (cmd.argv.empty() && cmd.input.empty() && cmd.output.empty() && cmd.error.empty()) {
        error = "empty command in pipeline"; return false;
    }
    return true;
}

bool parseJob(const std::vector<Token>& tokens, bool background, Job& job, std::string& error) {
    job = Job{};
    job.background = background;
    std::vector<Token> part;
    for (const auto& token : tokens) {
        if (token.type == Token::Type::Pipe) {
            if (part.empty()) { error = "missing command before '|'"; return false; }
            Command cmd;
            if (!parseCommand(part, cmd, error)) return false;
            job.commands.push_back(std::move(cmd));
            part.clear();
        } else part.push_back(token);
    }
    if (part.empty()) { error = "missing command after '|'"; return false; }
    Command cmd;
    if (!parseCommand(part, cmd, error)) return false;
    job.commands.push_back(std::move(cmd));
    return true;
}

bool isBuiltin(const std::string& name) {
    static const std::vector<std::string> names = {
        "help", "exit", "quit", "cd", "pwd", "history", "recall", "capsule", "croot", "back", "clear", "version", "env", "export"
    };
    return std::find(names.begin(), names.end(), name) != names.end();
}

class Shell {
public:
    Shell() : interactive_(isatty(STDIN_FILENO)) {
        std::error_code ec;
        fs::create_directories(configDir(), ec);
        fs::create_directories(dataDir(), ec);
        ::chmod(configDir().c_str(), 0700);
        ::chmod(dataDir().c_str(), 0700);
        loadConfig();
        loadHistory();
        // ALT-Shell exposes a shell-specific command journal protocol for companion tools.
        const std::string journal = (dataDir() / "recall.journal").string();
        ::setenv("ALT_SHELL_RECALL_PROTOCOL", "ALT-RECALL/1", 1);
        ::setenv("ALT_SHELL_RECALL_JOURNAL", journal.c_str(), 1);
        if (interactive_) {
            std::signal(SIGINT, SIG_IGN);
            std::signal(SIGQUIT, SIG_IGN);
        }
    }

    int run() {
        while (!shouldExit_) {
            auto input = readCommandLine();
            if (!input.has_value()) break;
            const std::string& line = *input;
            if (trim(line).empty()) continue;
            addHistory(line);
            std::string error;
            executeLine(line, error);
            if (!error.empty()) { std::cerr << "alt: " << error << '\n'; lastStatus_ = 2; }
        }
        saveHistory();
        return lastStatus_;
    }
private:
    bool interactive_ = false;
    bool shouldExit_ = false;
    int lastStatus_ = 0;
    int requestedExit_ = 0;
    std::vector<std::string> history_;
    std::vector<std::string> previousDirs_;
    ShellConfig config_;

    void loadConfig() {
        const fs::path file = configDir() / "config";
        if (!fs::exists(file)) {
            std::ofstream out(file);
            out << "# ALT-Shell configuration. One key=value per line; # starts a comment.\n"
                << "# External commands use bubblewrap when container.enabled=true.\n"
                << "container.enabled=true\n"
                << "container.network=false\n"
                << "container.workspace_writable=true\n"
                << "container.readonly_paths=/usr,/etc,/bin,/sbin,/lib,/lib64\n"
                << "container.extra_readonly=\n"
                << "container.extra_writable=\n"
                << "container.allowed_commands=\n"
                << "container.pass_env=TERM,LANG,LC_ALL,USER,LOGNAME,EDITOR\n";
            out.close();
            ::chmod(file.c_str(), 0600);
        }
        std::ifstream in(file);
        std::string line;
        while (std::getline(in, line)) {
            const auto comment = line.find('#');
            if (comment != std::string::npos) line.erase(comment);
            line = trim(line);
            if (line.empty()) continue;
            const auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = trim(line.substr(0, eq));
            const std::string value = trim(line.substr(eq + 1));
            if (key == "container.enabled") config_.containerEnabled = parseBool(value, config_.containerEnabled);
            else if (key == "container.network") config_.containerNetwork = parseBool(value, config_.containerNetwork);
            else if (key == "container.workspace_writable") config_.workspaceWritable = parseBool(value, config_.workspaceWritable);
            else if (key == "container.readonly_paths") config_.readonlyPaths = splitCsv(value);
            else if (key == "container.extra_readonly") config_.extraReadonlyPaths = splitCsv(value);
            else if (key == "container.extra_writable") config_.extraWritablePaths = splitCsv(value);
            else if (key == "container.allowed_commands") config_.allowedCommands = splitCsv(value);
            else if (key == "container.pass_env") config_.passEnvironment = splitCsv(value);
        }
        for (auto* list : {&config_.readonlyPaths, &config_.extraReadonlyPaths, &config_.extraWritablePaths}) {
            for (auto& path : *list) path = expandConfigPath(path);
        }
    }

    std::optional<std::string> readCommandLine() {
        if (!interactive_) {
            std::string line;
            if (std::getline(std::cin, line)) return line;
            return std::nullopt;
        }
        termios original{};
        if (::tcgetattr(STDIN_FILENO, &original) != 0) {
            std::cout << prompt() << std::flush;
            std::string line;
            if (std::getline(std::cin, line)) return line;
            return std::nullopt;
        }
        termios raw = original;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ISIG));
        raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL));
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) return std::nullopt;
        struct RestoreTerminal {
            termios old;
            ~RestoreTerminal() { ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &old); }
        } restore{original};

        std::string line;
        size_t cursor = 0;
        size_t historyPos = history_.size();
        std::string draft;
        auto redraw = [&]() {
            std::cout << '\r' << "\033[2K" << prompt() << line;
            const size_t right = utf8Columns(line, line.size()) - utf8Columns(line, cursor);
            if (right) std::cout << "\033[" << right << 'D';
            std::cout << std::flush;
        };
        redraw();
        while (true) {
            char ch = 0;
            const ssize_t got = ::read(STDIN_FILENO, &ch, 1);
            if (got <= 0) { std::cout << "\r\n"; return std::nullopt; }
            if (ch == '\r' || ch == '\n') { std::cout << "\r\n" << std::flush; return line; }
            if (ch == 3) { lastStatus_ = 130; line.clear(); std::cout << "^C\r\n"; return line; }
            if (ch == 4) {
                if (line.empty()) { std::cout << "\r\n"; return std::nullopt; }
                if (cursor < line.size()) line.erase(cursor, nextUtf8Boundary(line, cursor) - cursor);
                redraw(); continue;
            }
            if (ch == 1) { cursor = 0; redraw(); continue; }
            if (ch == 5) { cursor = line.size(); redraw(); continue; }
            if (ch == 11) { line.erase(cursor); redraw(); continue; }
            if (ch == 21) { line.erase(0, cursor); cursor = 0; redraw(); continue; }
            if (ch == 12) { std::cout << "\033[2J\033[H"; redraw(); continue; }
            if (ch == 23) {
                while (cursor > 0 && line[cursor - 1] == ' ') cursor = previousUtf8Boundary(line, cursor);
                while (cursor > 0 && line[cursor - 1] != ' ') cursor = previousUtf8Boundary(line, cursor);
                line.erase(cursor, line.size() - cursor);
                redraw(); continue;
            }
            if (ch == 127 || ch == 8) {
                if (cursor > 0) { size_t prev = previousUtf8Boundary(line, cursor); line.erase(prev, cursor - prev); cursor = prev; }
                redraw(); continue;
            }
            if (ch == 27) {
                pollfd pfd{STDIN_FILENO, POLLIN, 0};
                if (::poll(&pfd, 1, 40) <= 0) continue;
                char seq1 = 0;
                if (::read(STDIN_FILENO, &seq1, 1) != 1 || seq1 != '[') continue;
                std::string seq;
                while (seq.size() < 8) {
                    pollfd pfd2{STDIN_FILENO, POLLIN, 0};
                    if (::poll(&pfd2, 1, 40) <= 0) break;
                    char part = 0;
                    if (::read(STDIN_FILENO, &part, 1) != 1) break;
                    seq += part;
                    if ((part >= 'A' && part <= 'Z') || part == '~') break;
                }
                if (seq == "A") {
                    if (historyPos > 0) {
                        if (historyPos == history_.size()) draft = line;
                        --historyPos; line = history_[historyPos]; cursor = line.size();
                    }
                } else if (seq == "B") {
                    if (historyPos < history_.size()) {
                        ++historyPos;
                        if (historyPos == history_.size()) line = draft;
                        else line = history_[historyPos];
                        cursor = line.size();
                    }
                } else if (seq == "C") {
                    cursor = nextUtf8Boundary(line, cursor);
                } else if (seq == "D") {
                    cursor = previousUtf8Boundary(line, cursor);
                } else if (seq == "H" || seq == "1~" || seq == "7~") {
                    cursor = 0;
                } else if (seq == "F" || seq == "4~" || seq == "8~") {
                    cursor = line.size();
                } else if (seq == "3~" && cursor < line.size()) {
                    line.erase(cursor, nextUtf8Boundary(line, cursor) - cursor);
                }
                redraw(); continue;
            }
            if (static_cast<unsigned char>(ch) >= 32) {
                line.insert(cursor, 1, ch);
                ++cursor;
                redraw();
            }
        }
    }

    std::string prompt() const {
        char host[256] = "machine";
        gethostname(host, sizeof(host) - 1);
        const char* user = std::getenv("USER");
        std::error_code ec;
        std::string cwd = fs::current_path(ec).string();
        const std::string home = homeDir();
        if (cwd == home) cwd = "~";
        else if (cwd.rfind(home + "/", 0) == 0) cwd = "~" + cwd.substr(home.size());
        std::ostringstream out;
        out << "\033[1;36m" << (user ? user : "user") << "@" << host << "\033[0m "
            << "\033[1;34m" << cwd << "\033[0m";
        if (lastStatus_ != 0) out << " \033[1;31m" << lastStatus_ << "\033[0m";
        out << " ❯ ";
        return out.str();
    }

    // Private companion-tool ABI: ALT-RECALL/1 tab-separated records.
    // Fields: protocol, event, unix-ms, status, hex(cwd), hex(command).
    // Hex encoding keeps arbitrary command text and paths unambiguous.
    void journalEvent(const char* event, const std::string& command, int status) const {
        const fs::path file = dataDir() / "recall.journal";
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::ofstream out(file, std::ios::app);
        if (!out) return; // Observability must never prevent a command from running.
        out << "ALT-RECALL/1\t" << event << '\t' << now << '\t' << status << '\t'
            << hexEncode(fs::current_path().string()) << '\t' << hexEncode(command) << '\n';
        out.close();
        ::chmod(file.c_str(), 0600);
    }

    void loadHistory() {
        std::ifstream in(dataDir() / "history");
        std::string line;
        while (std::getline(in, line)) {
            history_.push_back(line);
            if (history_.size() > 2000) history_.erase(history_.begin());
        }
    }

    void addHistory(const std::string& line) {
        if (!history_.empty() && history_.back() == line) return;
        history_.push_back(line);
        if (history_.size() > 2000) history_.erase(history_.begin());
        std::ofstream out(dataDir() / "history", std::ios::app);
        if (out) out << line << '\n';
    }

    void saveHistory() {
        std::ofstream out(dataDir() / "history", std::ios::trunc);
        if (!out) return;
        const size_t begin = history_.size() > 2000 ? history_.size() - 2000 : 0;
        for (size_t i = begin; i < history_.size(); ++i) out << history_[i] << '\n';
        ::chmod((dataDir() / "history").c_str(), 0600);
    }

    void executeChunk(const std::string& raw, bool background, std::string& error) {
        if (trim(raw).empty()) {
            if (background) error = "'&' must follow a command";
            return;
        }
        std::vector<Token> tokens;
        if (!lexLine(raw, lastStatus_, tokens, error)) return;
        Job job;
        if (!parseJob(tokens, background, job, error)) return;
        journalEvent("BEGIN", raw, -1);
        lastStatus_ = executeJob(job);
        journalEvent("END", raw, lastStatus_);
    }

    void executeLine(const std::string& line, std::string& error) {
        enum class Quote { None, Single, Double } quote = Quote::None;
        enum class Next { Always, OnSuccess, OnFailure } next = Next::Always;
        size_t begin = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (quote == Quote::Single) {
                if (c == '\'') quote = Quote::None;
                continue;
            }
            if (quote == Quote::Double) {
                if (c == '\\' && i + 1 < line.size()) {
                    ++i;
                    continue;
                }
                if (c == '"') quote = Quote::None;
                continue;
            }
            if (c == '\'') {
                quote = Quote::Single;
                continue;
            }
            if (c == '"') {
                quote = Quote::Double;
                continue;
            }

            Next separator = Next::Always;
            size_t separatorLength = 0;
            if (c == ';') {
                separator = Next::Always;
                separatorLength = 1;
            } else if (c == '&' && i + 1 < line.size() && line[i + 1] == '&') {
                separator = Next::OnSuccess;
                separatorLength = 2;
            } else if (c == '|' && i + 1 < line.size() && line[i + 1] == '|') {
                separator = Next::OnFailure;
                separatorLength = 2;
            }
            if (separatorLength == 0) continue;

            const bool shouldRun = next == Next::Always ||
                (next == Next::OnSuccess && lastStatus_ == 0) ||
                (next == Next::OnFailure && lastStatus_ != 0);
            if (shouldRun) executeChunk(line.substr(begin, i - begin), false, error);
            next = separator;
            i += separatorLength - 1;
            begin = i + 1;
        }
        const bool shouldRun = next == Next::Always ||
            (next == Next::OnSuccess && lastStatus_ == 0) ||
            (next == Next::OnFailure && lastStatus_ != 0);
        if (shouldRun) executeChunk(line.substr(begin), false, error);
    }

    int runBuiltin(const std::vector<std::string>& a) {
        if (a.empty()) return 0;
        const std::string& name = a[0];
        if (name == "help") {
            std::cout << "ALT-Shell " << VERSION << " — a Linux shell with context capsules and sandboxing\n\n"
                      << "Interactive editing: Up/Down history, Left/Right cursor, Home/End, Delete/Backspace, Ctrl+A/E/U/K/W/L.\n"
                      << "Config: ~/.config/alt-shell/config (respects XDG_CONFIG_HOME).\n"
                      << "External commands use bubblewrap by default; missing bwrap fails closed.\n\n"
                      << "Commands:\n"
                      << "  cd [DIR]              change directory (default: HOME)\n"
                      << "  croot                 jump to the nearest parent containing .git\n"
                      << "  back                  return to the previous directory\n"
                      << "  capsule save NAME [VARS...]  save cwd and selected environment variables\n"
                      << "  capsule load NAME           restore a saved context\n"
                      << "  capsule list                list saved contexts\n"
                      << "  capsule show NAME           inspect a context\n"
                      << "  capsule diff NAME           compare it with the current context\n"
                      << "  history [N]           show recent commands\n"
                      << "  recall TEXT           find commands in history containing TEXT\n"
                      << "  sentinel [COMMAND]    inspect Linux kernel/system telemetry (private companion)\n"
                      << "  Companion API: ALT_SHELL_RECALL_PROTOCOL=ALT-RECALL/1\n"
                      << "  Event journal: $XDG_DATA_HOME/alt-shell/recall.journal (mode 0600)\n"
                      << "  env                   print environment variables\n"
                      << "  export NAME=VALUE     set an environment variable for this shell\n"
                      << "  pwd, clear, version, help, exit\n\n"
                      << "Syntax: pipelines (|), input/output redirection (<, >, >>, 2>), ; and background (&).\n"
                      << "Expansions: $NAME, braced variables, $?, $, and ~. No job control or command substitution yet.\n"
                      << "Sandbox options: network, writable workspace, extra mounts, environment and command allowlists.\n";
            return 0;
        }
        if (name == "version") { std::cout << "ALT-Shell " << VERSION << " (C++17 / POSIX)\n"; return 0; }
        if (name == "exit" || name == "quit") {
            shouldExit_ = true;
            if (a.size() > 1) {
                try { requestedExit_ = std::stoi(a[1]) & 255; } catch (...) { requestedExit_ = 2; }
            } else requestedExit_ = lastStatus_;
            return requestedExit_;
        }
        if (name == "clear") { std::cout << "\033[2J\033[H"; return 0; }
        if (name == "pwd") { std::error_code ec; std::cout << fs::current_path(ec).string() << '\n'; return ec ? 1 : 0; }
        if (name == "cd") {
            const std::string target = a.size() > 1 ? (a[1] == "-" ? (previousDirs_.empty() ? "" : previousDirs_.back()) : a[1]) : homeDir();
            if (target.empty()) { std::cerr << "cd: no previous directory\n"; return 1; }
            std::error_code ec;
            const std::string old = fs::current_path(ec).string();
            if (::chdir(target.c_str()) != 0) { std::cerr << "cd: " << target << ": " << std::strerror(errno) << '\n'; return 1; }
            previousDirs_.push_back(old);
            if (a.size() > 1 && a[1] == "-") std::cout << fs::current_path().string() << '\n';
            return 0;
        }
        if (name == "back") {
            if (previousDirs_.empty()) { std::cerr << "back: directory history is empty\n"; return 1; }
            std::string target = previousDirs_.back();
            if (::chdir(target.c_str()) != 0) { std::cerr << "back: " << target << ": " << std::strerror(errno) << '\n'; return 1; }
            previousDirs_.pop_back();
            return 0;
        }
        if (name == "croot") {
            fs::path p = fs::current_path();
            while (true) {
                if (fs::exists(p / ".git")) {
                    std::string old = fs::current_path().string();
                    if (::chdir(p.c_str()) == 0) { previousDirs_.push_back(old); std::cout << p.string() << '\n'; return 0; }
                    std::cerr << "croot: " << std::strerror(errno) << '\n'; return 1;
                }
                if (p == p.root_path()) break;
                p = p.parent_path();
            }
            std::cerr << "croot: no .git directory in this path's parents\n"; return 1;
        }
        if (name == "history") {
            size_t count = 20;
            if (a.size() > 1) { try { count = static_cast<size_t>(std::stoul(a[1])); } catch (...) { std::cerr << "history: expected a number\n"; return 2; } }
            const size_t begin = history_.size() > count ? history_.size() - count : 0;
            for (size_t i = begin; i < history_.size(); ++i) std::cout << (i + 1) << "  " << history_[i] << '\n';
            return 0;
        }
        if (name == "sentinel") {
            // The proprietary kernel-observation companion is intentionally distributed separately.
            // It reads Linux interfaces as the current user and must not be run through the shell sandbox.
            static const std::vector<std::string> allowed = {
                "status", "memory", "processes", "events", "explain", "watch", "doctor", "help"
            };
            if (a.size() > 1 && std::find(allowed.begin(), allowed.end(), a[1]) == allowed.end()) {
                std::cerr << "sentinel: unsupported subcommand: " << a[1] << "\n";
                std::cerr << "Usage: sentinel [status|memory|processes|events|explain|watch|doctor|help]\n";
                return 2;
            }
            const fs::path companion = dataDir() / "sentinel";
            struct stat st{};
            if (::stat(companion.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
                st.st_uid != getuid() || (st.st_mode & (S_IWGRP | S_IWOTH)) ||
                !(st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH))) {
                std::cerr << "sentinel: proprietary companion is missing or has unsafe permissions: "
                          << companion << "\n";
                std::cerr << "Install the private sentinel executable there with mode 0700.\n";
                return 127;
            }
            pid_t pid = ::fork();
            if (pid < 0) { std::cerr << "sentinel: fork: " << std::strerror(errno) << '\n'; return 1; }
            if (pid == 0) {
                std::string companionArg0 = companion.string();
                std::vector<char*> args;
                args.push_back(companionArg0.data());
                for (size_t i = 1; i < a.size(); ++i) args.push_back(const_cast<char*>(a[i].c_str()));
                args.push_back(nullptr);
                ::execv(companion.c_str(), args.data());
                std::cerr << "sentinel: exec: " << std::strerror(errno) << '\n';
                _exit(127);
            }
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0) {
                if (errno == EINTR) continue;
                std::cerr << "sentinel: waitpid: " << std::strerror(errno) << '\n';
                return 1;
            }
            if (WIFEXITED(status)) return WEXITSTATUS(status);
            if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
            return 1;
        }
        if (name == "recall") {
            if (a.size() >= 2 && (a[1] == "timeline" || a[1] == "search" || a[1] == "last" ||
                                  a[1] == "status" || a[1] == "checkpoint" || a[1] == "restore" ||
                                  a[1] == "undo")) {
                // Companion runs from the per-user ALT-Shell data directory; it consumes only
                // the shell's journal protocol and never executes the recorded commands.
                const fs::path companion = dataDir() / "recall";
                struct stat st{};
                if (::stat(companion.c_str(), &st) != 0 || !S_ISREG(st.st_mode) ||
                    st.st_uid != getuid() || (st.st_mode & (S_IWGRP | S_IWOTH))) {
                    std::cerr << "recall: proprietary companion is missing or has unsafe permissions: "
                              << companion << "\n";
                    std::cerr << "Build the private recall.cpp and install it there with mode 0700.\n";
                    return 127;
                }
                pid_t pid = ::fork();
                if (pid < 0) { std::cerr << "recall: fork: " << std::strerror(errno) << '\n'; return 1; }
                if (pid == 0) {
                    std::string companionArg0 = companion.string();
                    std::vector<char*> args;
                    args.push_back(companionArg0.data());
                    for (size_t i = 1; i < a.size(); ++i) args.push_back(const_cast<char*>(a[i].c_str()));
                    args.push_back(nullptr);
                    ::execv(companion.c_str(), args.data());
                    _exit(errno == ENOENT ? 127 : 126);
                }
                int status = 0;
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
                if (WIFEXITED(status)) return WEXITSTATUS(status);
                if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
                return 1;
            }
            if (a.size() < 2) { std::cerr << "usage: recall TEXT | recall timeline|search|last|status|checkpoint|restore|undo ...\n"; return 2; }
            std::string needle;
            for (size_t i = 1; i < a.size(); ++i) { if (!needle.empty()) needle += ' '; needle += a[i]; }
            size_t found = 0;
            for (size_t i = 0; i < history_.size(); ++i) if (history_[i].find(needle) != std::string::npos) {
                std::cout << (i + 1) << "  " << history_[i] << '\n'; ++found;
            }
            if (!found) std::cout << "No history entries contain: " << needle << '\n';
            return 0;
        }
        if (name == "env") {
            for (char** e = ::environ; e && *e; ++e) std::cout << *e << '\n';
            return 0;
        }
        if (name == "export") {
            if (a.size() < 2) { std::cerr << "usage: export NAME=VALUE\n"; return 2; }
            for (size_t i = 1; i < a.size(); ++i) {
                const auto pos = a[i].find('=');
                if (pos == std::string::npos) { std::cerr << "export: expected NAME=VALUE: " << a[i] << '\n'; return 2; }
                std::string key = a[i].substr(0, pos), value = a[i].substr(pos + 1);
                if (!validName(key)) { std::cerr << "export: invalid variable name: " << key << '\n'; return 2; }
                if (setenv(key.c_str(), value.c_str(), 1) != 0) { std::cerr << "export: " << std::strerror(errno) << '\n'; return 1; }
            }
            return 0;
        }
        if (name == "capsule") return capsule(a);
        return 127;
    }

    struct Capsule {
        std::string cwd;
        std::map<std::string, std::pair<bool, std::string>> vars;
    };

    fs::path capsulePath(const std::string& name) const { return configDir() / (name + ".capsule"); }

    bool readCapsule(const std::string& name, Capsule& c) {
        if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos || name == "." || name == "..") {
            std::cerr << "capsule: invalid name\n"; return false;
        }
        std::ifstream in(capsulePath(name));
        if (!in) { std::cerr << "capsule: no such capsule: " << name << '\n'; return false; }
        std::string line;
        if (!std::getline(in, line) || line != "ALT_CAPSULE_1") { std::cerr << "capsule: invalid or unsupported file format\n"; return false; }
        Capsule temp;
        while (std::getline(in, line)) {
            std::istringstream row(line);
            std::string kind, x, y, z;
            row >> kind;
            if (kind == "PWD") {
                row >> x;
                if (!hexDecode(x, temp.cwd)) { std::cerr << "capsule: corrupted PWD field\n"; return false; }
            } else if (kind == "ENV") {
                row >> x >> y >> z;
                if (!validName(x) || (y != "SET" && y != "UNSET")) { std::cerr << "capsule: corrupted ENV field\n"; return false; }
                std::string value;
                if (y == "SET" && !hexDecode(z, value)) { std::cerr << "capsule: corrupted variable value\n"; return false; }
                temp.vars[x] = {y == "SET", value};
            }
        }
        if (temp.cwd.empty()) { std::cerr << "capsule: missing working directory\n"; return false; }
        c = std::move(temp);
        return true;
    }

    int capsule(const std::vector<std::string>& a) {
        if (a.size() < 2 || a[1] == "help") {
            std::cout << "Usage:\n  capsule save NAME [ENV_VAR ...]\n  capsule load NAME\n  capsule list\n  capsule show NAME\n  capsule diff NAME\n\nOnly variables explicitly listed in 'save' are stored; this avoids accidentally persisting API keys and tokens.\n";
            return a.size() < 2 ? 2 : 0;
        }
        const std::string action = a[1];
        if (action == "list") {
            std::error_code ec;
            fs::create_directories(configDir(), ec);
            bool any = false;
            for (const auto& e : fs::directory_iterator(configDir(), ec)) {
                if (e.is_regular_file() && e.path().extension() == ".capsule") {
                    std::cout << e.path().stem().string() << '\n'; any = true;
                }
            }
            if (!any) std::cout << "No capsules yet. Try: capsule save work EDITOR\n";
            return 0;
        }
        if (action == "save") {
            if (a.size() < 3) { std::cerr << "usage: capsule save NAME [ENV_VAR ...]\n"; return 2; }
            const std::string& name = a[2];
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") != std::string::npos || name == "." || name == "..") {
                std::cerr << "capsule: invalid name\n"; return 2;
            }
            for (size_t i = 3; i < a.size(); ++i) {
                if (!validName(a[i])) { std::cerr << "capsule: invalid variable name: " << a[i] << '\n'; return 2; }
            }
            const fs::path file = capsulePath(name);
            const std::string cwd = fs::current_path().string();
            std::ofstream out(file, std::ios::trunc);
            if (!out) { std::cerr << "capsule: cannot write " << file << '\n'; return 1; }
            out << "ALT_CAPSULE_1\nPWD " << hexEncode(cwd) << '\n';
            for (size_t i = 3; i < a.size(); ++i) {
                const char* val = std::getenv(a[i].c_str());
                out << "ENV " << a[i] << ' ' << (val ? "SET " : "UNSET ") << (val ? hexEncode(val) : "-") << '\n';
            }
            out.close();
            if (!out) { std::cerr << "capsule: write failed\n"; return 1; }
            ::chmod(file.c_str(), 0600);
            std::cout << "Saved capsule '" << name << "' (" << cwd << ", " << (a.size() - 3) << " variables).\n";
            return 0;
        }
        if (action == "load") {
            if (a.size() != 3) { std::cerr << "usage: capsule load NAME\n"; return 2; }
            Capsule c;
            if (!readCapsule(a[2], c)) return 1;
            std::error_code ec;
            if (!fs::is_directory(c.cwd, ec)) { std::cerr << "capsule: saved directory is missing: " << c.cwd << '\n'; return 1; }
            const std::string old = fs::current_path().string();
            if (::chdir(c.cwd.c_str()) != 0) { std::cerr << "capsule: cannot enter directory: " << std::strerror(errno) << '\n'; return 1; }
            previousDirs_.push_back(old);
            for (const auto& [key, value] : c.vars) {
                if (value.first) setenv(key.c_str(), value.second.c_str(), 1);
                else unsetenv(key.c_str());
            }
            std::cout << "Loaded capsule '" << a[2] << "' -> " << c.cwd << '\n';
            return 0;
        }
        if (action == "show" || action == "diff") {
            if (a.size() != 3) { std::cerr << "usage: capsule " << action << " NAME\n"; return 2; }
            Capsule c;
            if (!readCapsule(a[2], c)) return 1;
            const std::string cwd = fs::current_path().string();
            if (action == "show") {
                std::cout << "Capsule: " << a[2] << "\nDirectory: " << c.cwd << '\n';
                if (c.vars.empty()) std::cout << "Variables: (none)\n";
                else for (const auto& [key, value] : c.vars) std::cout << key << "=" << (value.first ? value.second : "<unset>") << '\n';
                return 0;
            }
            bool changed = false;
            if (cwd != c.cwd) { std::cout << "PWD: current=" << cwd << " | saved=" << c.cwd << '\n'; changed = true; }
            for (const auto& [key, value] : c.vars) {
                const char* current = std::getenv(key.c_str());
                const bool present = current != nullptr;
                if (present != value.first || (present && value.second != current)) {
                    std::cout << key << ": current=" << (present ? current : "<unset>") << " | saved=" << (value.first ? value.second : "<unset>") << '\n';
                    changed = true;
                }
            }
            if (!changed) std::cout << "Current context matches capsule '" << a[2] << "'.\n";
            return 0;
        }
        std::cerr << "capsule: unknown action: " << action << " (try 'capsule help')\n";
        return 2;
    }


    bool pathIsWithin(const fs::path& root, const fs::path& candidate) const {
        const fs::path rel = candidate.lexically_relative(root);
        if (rel.empty()) return candidate == root;
        return !rel.is_absolute() && *rel.begin() != "..";
    }

    bool validateRedirections(const Job& job) const {
        if (!config_.containerEnabled) return true;
        std::error_code ec;
        const fs::path root = fs::weakly_canonical(fs::current_path(), ec);
        if (ec) { std::cerr << "alt: sandbox: cannot resolve current directory\n"; return false; }
        for (const auto& cmd : job.commands) {
            for (const auto& redirect : {std::pair<const std::string*, bool>{&cmd.input, false}, {&cmd.output, true}, {&cmd.error, true}}) {
                const std::string& name = *redirect.first;
                if (name.empty()) continue;
                if (redirect.second && !config_.workspaceWritable) {
                    std::cerr << "alt: sandbox: output redirection disabled by container.workspace_writable=false\n";
                    return false;
                }
                fs::path target = fs::path(name);
                if (target.is_relative()) target = root / target;
                target = target.lexically_normal();
                ec.clear();
                fs::path resolved;
                if (fs::exists(target, ec)) {
                    resolved = fs::canonical(target, ec);
                } else {
                    ec.clear();
                    resolved = fs::weakly_canonical(target.parent_path(), ec) / target.filename();
                }
                if (ec || !pathIsWithin(root, resolved.lexically_normal())) {
                    std::cerr << "alt: sandbox: redirections must stay inside the current workspace: " << name << '\n';
                    return false;
                }
            }
        }
        return true;
    }

    bool commandAllowed(const Command& cmd) const {
        if (cmd.argv.empty() || config_.allowedCommands.empty() || isBuiltin(cmd.argv[0])) return true;
        const std::string base = fs::path(cmd.argv[0]).filename().string();
        return std::find(config_.allowedCommands.begin(), config_.allowedCommands.end(), base) != config_.allowedCommands.end();
    }

    [[noreturn]] void execSandboxed(const Command& cmd) const {
        const fs::path cwd = fs::current_path();
        const fs::path home = fs::weakly_canonical(homeDir());
        if (fs::weakly_canonical(cwd) == home) {
            std::cerr << "alt: sandbox: refusing to expose the entire home directory; cd into a project/work directory first\n";
            _exit(126);
        }
        for (const auto& protectedPath : config_.readonlyPaths) {
            std::error_code ec;
            const fs::path protectedCanonical = fs::weakly_canonical(protectedPath, ec);
            if (!ec && pathIsWithin(protectedCanonical, fs::weakly_canonical(cwd))) {
                std::cerr << "alt: sandbox: current directory is inside a read-only system mount; cd into a project/work directory first\n";
                _exit(126);
            }
        }
        std::vector<std::string> args = {
            "bwrap", "--die-with-parent", "--new-session", "--unshare-user", "--unshare-pid",
            "--unshare-ipc", "--unshare-uts", "--disable-userns"
        };
        if (!config_.containerNetwork) args.push_back("--unshare-net");
        args.insert(args.end(), {"--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp"});

        auto bindReadonly = [&](const std::string& source, const std::string& destination) {
            std::error_code ec;
            if (fs::is_directory(source, ec)) args.insert(args.end(), {"--ro-bind", source, destination});
        };
        for (const auto& path : config_.readonlyPaths) bindReadonly(path, path);
        args.insert(args.end(), {"--dir", "/workspace"});
        args.push_back(config_.workspaceWritable ? "--bind" : "--ro-bind");
        args.push_back(cwd.string()); args.push_back("/workspace");

        auto mountExtra = [&](const std::vector<std::string>& paths, bool writable, const std::string& prefix) {
            if (paths.empty()) return;
            args.insert(args.end(), {"--dir", "/mnt"});
            size_t i = 0;
            for (const auto& path : paths) {
                std::error_code ec;
                if (!fs::is_directory(path, ec)) {
                    std::cerr << "alt: sandbox: configured mount is not a directory or is missing: " << path << '\n';
                    _exit(126);
                }
                const std::string destination = prefix + std::to_string(i++);
                args.insert(args.end(), {"--dir", destination, writable ? "--bind" : "--ro-bind", path, destination});
            }
        };
        mountExtra(config_.extraReadonlyPaths, false, "/mnt/alt-ro-");
        mountExtra(config_.extraWritablePaths, true, "/mnt/alt-rw-");
        args.insert(args.end(), {"--chdir", "/workspace", "--clearenv", "--setenv", "PATH",
                                "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/bin:/sbin",
                                "--setenv", "HOME", "/workspace"});
        for (const auto& key : config_.passEnvironment) {
            if (!validName(key) || key == "HOME" || key == "PATH") continue;
            const char* value = std::getenv(key.c_str());
            if (value) args.insert(args.end(), {"--setenv", key, value});
        }
        args.push_back("--");
        for (const auto& arg : cmd.argv) args.push_back(arg);
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& arg : args) argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        if (errno == ENOENT) {
            std::cerr << "alt: bubblewrap (bwrap) is missing; external commands are blocked while container.enabled=true. Install bubblewrap or explicitly set container.enabled=false.\n";
        } else {
            std::cerr << "alt: cannot start sandbox: " << std::strerror(errno) << '\n';
        }
        _exit(errno == ENOENT ? 127 : 126);
    }

    int executeJob(const Job& job) {
        if (!validateRedirections(job)) return 126;
        for (const auto& cmd : job.commands) {
            if (config_.containerEnabled && !commandAllowed(cmd)) {
                std::cerr << "alt: blocked by container.allowed_commands: " << cmd.argv.front() << '\n';
                return 126;
            }
        }
        if (job.commands.size() == 1 && !job.background && !job.commands[0].argv.empty() && isBuiltin(job.commands[0].argv[0])) {
            const auto& cmd = job.commands[0];
            // Parent-run builtins may still redirect their output.
            int oldIn = -1, oldOut = -1, oldErr = -1;
            auto restoreFd = [](int fd, int old) { if (old >= 0) { ::dup2(old, fd); ::close(old); } };
            if (!cmd.input.empty()) {
                oldIn = ::dup(STDIN_FILENO);
                int fd = ::open(cmd.input.c_str(), O_RDONLY);
                if (fd < 0) { std::cerr << "alt: " << cmd.input << ": " << std::strerror(errno) << '\n'; ::close(oldIn); return 1; }
                ::dup2(fd, STDIN_FILENO); ::close(fd);
            }
            if (!cmd.output.empty()) {
                oldOut = ::dup(STDOUT_FILENO);
                int flags = O_WRONLY | O_CREAT | (cmd.append ? O_APPEND : O_TRUNC);
                int fd = ::open(cmd.output.c_str(), flags, 0666);
                if (fd < 0) { std::cerr << "alt: " << cmd.output << ": " << std::strerror(errno) << '\n'; restoreFd(STDIN_FILENO, oldIn); ::close(oldOut); return 1; }
                ::dup2(fd, STDOUT_FILENO); ::close(fd);
            }
            if (!cmd.error.empty()) {
                oldErr = ::dup(STDERR_FILENO);
                int fd = ::open(cmd.error.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) { std::cerr << "alt: " << cmd.error << ": " << std::strerror(errno) << '\n'; restoreFd(STDIN_FILENO, oldIn); restoreFd(STDOUT_FILENO, oldOut); ::close(oldErr); return 1; }
                ::dup2(fd, STDERR_FILENO); ::close(fd);
            }
            int result = runBuiltin(cmd.argv);
            std::cout.flush(); std::cerr.flush();
            restoreFd(STDIN_FILENO, oldIn); restoreFd(STDOUT_FILENO, oldOut); restoreFd(STDERR_FILENO, oldErr);
            if (shouldExit_) return requestedExit_;
            return result;
        }

        const size_t n = job.commands.size();
        std::vector<std::array<int, 2>> pipes(n > 1 ? n - 1 : 0);
        for (auto& p : pipes) {
            if (::pipe(p.data()) < 0) { std::cerr << "alt: pipe: " << std::strerror(errno) << '\n'; return 1; }
        }
        std::vector<pid_t> pids;
        for (size_t i = 0; i < n; ++i) {
            pid_t pid = ::fork();
            if (pid < 0) {
                std::cerr << "alt: fork: " << std::strerror(errno) << '\n';
                for (auto& p : pipes) { ::close(p[0]); ::close(p[1]); }
                for (pid_t child : pids) ::waitpid(child, nullptr, 0);
                return 1;
            }
            if (pid == 0) {
                std::signal(SIGINT, SIG_DFL); std::signal(SIGQUIT, SIG_DFL);
                if (i > 0) ::dup2(pipes[i - 1][0], STDIN_FILENO);
                else if (job.background && job.commands[i].input.empty()) {
                    int nullfd = ::open("/dev/null", O_RDONLY);
                    if (nullfd >= 0) { ::dup2(nullfd, STDIN_FILENO); ::close(nullfd); }
                }
                if (i + 1 < n) ::dup2(pipes[i][1], STDOUT_FILENO);
                for (auto& p : pipes) { ::close(p[0]); ::close(p[1]); }
                const Command& cmd = job.commands[i];
                if (!cmd.input.empty()) {
                    int fd = ::open(cmd.input.c_str(), O_RDONLY);
                    if (fd < 0) { std::cerr << "alt: " << cmd.input << ": " << std::strerror(errno) << '\n'; _exit(1); }
                    ::dup2(fd, STDIN_FILENO); ::close(fd);
                }
                if (!cmd.output.empty()) {
                    int flags = O_WRONLY | O_CREAT | (cmd.append ? O_APPEND : O_TRUNC);
                    int fd = ::open(cmd.output.c_str(), flags, 0666);
                    if (fd < 0) { std::cerr << "alt: " << cmd.output << ": " << std::strerror(errno) << '\n'; _exit(1); }
                    ::dup2(fd, STDOUT_FILENO); ::close(fd);
                }
                if (!cmd.error.empty()) {
                    int fd = ::open(cmd.error.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                    if (fd < 0) { std::cerr << "alt: " << cmd.error << ": " << std::strerror(errno) << '\n'; _exit(1); }
                    ::dup2(fd, STDERR_FILENO); ::close(fd);
                }
                if (cmd.argv.empty()) _exit(0);
                if (config_.containerEnabled && cmd.argv[0] == "env") {
                    for (const auto& key : config_.passEnvironment) {
                        const char* value = std::getenv(key.c_str());
                        if (value) std::cout << key << '=' << value << '\n';
                    }
                    std::cout.flush();
                    _exit(0);
                }
                if (isBuiltin(cmd.argv[0])) _exit(runBuiltin(cmd.argv));
                std::vector<char*> args;
                for (const auto& s : cmd.argv) args.push_back(const_cast<char*>(s.c_str()));
                args.push_back(nullptr);
                if (config_.containerEnabled) execSandboxed(cmd);
                ::execvp(args[0], args.data());
                std::cerr << "alt: " << args[0] << ": " << std::strerror(errno) << '\n';
                _exit(errno == ENOENT ? 127 : 126);
            }
            pids.push_back(pid);
        }
        for (auto& p : pipes) { ::close(p[0]); ::close(p[1]); }
        if (job.background) {
            std::cout << "[background pid " << pids.front() << "]\n";
            return 0;
        }
        int status = 0;
        for (pid_t pid : pids) {
            int s = 0;
            while (::waitpid(pid, &s, 0) < 0 && errno == EINTR) {}
            if (pid == pids.back()) status = s;
        }
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return 1;
    }
};
} // namespace

int main() {
    Shell shell;
    return shell.run();
}
