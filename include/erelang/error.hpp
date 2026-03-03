#pragma once

#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace erelang {

enum class DiagnosticSeverity { Error, Warning, Note };

struct Diagnostic {
    std::string message;
    int line{0};
    int column{0};
    DiagnosticSeverity severity{DiagnosticSeverity::Error};
};

[[nodiscard]] std::string_view to_string(DiagnosticSeverity severity) noexcept;

class ErrorReporter {
public:
    ErrorReporter() = default;

    void report(DiagnosticSeverity severity, int line, int column, std::string message);
    void error(int line, int column, std::string message);
    void warn(int line, int column, std::string message);
    void note(int line, int column, std::string message);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::span<const Diagnostic> diagnostics() const noexcept;
    void clear() noexcept;

private:
    mutable std::mutex mutex_;
    std::vector<Diagnostic> diags_;
};

} // namespace erelang
