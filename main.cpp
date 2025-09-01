/**
 * System Monitor - A terminal-based system information display
 * 
 * This program displays real-time system information including:
 * - CPU usage percentage
 * - RAM usage percentage  
 * - Disk usage percentage
 * - Network transfer rates
 * - System uptime
 * - CPU temperature (if available)
 * - Hostname and current user
 * 
 * Uses ncurses for a clean terminal UI with Unicode box drawing characters.
 */

#include <iostream>
#include <locale.h>
#include <ncurses.h>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <thread>
#include <chrono>
#include <iomanip>
#include <unistd.h>
#include <sys/statvfs.h>
#include <pwd.h>

using ull = unsigned long long;

// =============================================================================
// SYSTEM INFORMATION FUNCTIONS
// =============================================================================

/**
 * Reads CPU usage percentage from /proc/stat
 * Uses delta calculation between calls to get accurate usage
 * @return CPU usage as percentage (0.0-100.0), or 0.0 on first call
 */
double get_cpu_usage() {
    static bool first_call = true;
    static ull last_total = 0, last_idle = 0;

    std::ifstream file("/proc/stat");
    if (!file.is_open()) {
        return -1.0; // Error reading file
    }

    std::string cpu_label;
    ull user, nice, system, idle, iowait, irq, softirq, steal;
    
    file >> cpu_label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;

    // Calculate total and idle time
    ull idle_time = idle + iowait;
    ull non_idle_time = user + nice + system + irq + softirq + steal;
    ull total_time = idle_time + non_idle_time;

    // First call: just store values, can't calculate usage yet
    if (first_call) {
        last_total = total_time;
        last_idle = idle_time;
        first_call = false;
        return 0.0;
    }

    // Calculate deltas since last call
    ull total_delta = total_time - last_total;
    ull idle_delta = idle_time - last_idle;

    // Store current values for next call
    last_total = total_time;
    last_idle = idle_time;

    // Avoid division by zero
    if (total_delta == 0) return 0.0;

    // Calculate CPU usage percentage
    return (100.0 * (total_delta - idle_delta)) / total_delta;
}

/**
 * Reads RAM usage percentage from /proc/meminfo
 * @return RAM usage as percentage (0.0-100.0), or -1.0 on error
 */
double get_ram_usage() {
    std::ifstream meminfo("/proc/meminfo");
    if (!meminfo.is_open()) {
        return -1.0;
    }

    std::string key, unit;
    unsigned long value;
    unsigned long mem_total = 0, mem_available = 0;

    // Parse meminfo file to find total and available memory
    while (meminfo >> key >> value >> unit) {
        if (key == "MemTotal:") {
            mem_total = value;
        } else if (key == "MemAvailable:") {
            mem_available = value;
            break; // We have both values we need
        }
    }

    if (mem_total == 0) return -1.0;

    // Calculate used memory and return as percentage
    double used_memory = mem_total - mem_available;
    return (used_memory * 100.0) / mem_total;
}

/**
 * Reads system uptime in seconds from /proc/uptime
 * @return Uptime in seconds, or 0.0 on error
 */
double get_uptime_seconds() {
    std::ifstream file("/proc/uptime");
    if (!file.is_open()) {
        return 0.0;
    }

    double uptime = 0.0;
    file >> uptime;
    return uptime;
}

/**
 * Reads disk usage percentage for a given path
 * @param path Filesystem path to check (default: root "/")
 * @return Disk usage as percentage (0.0-100.0), or -1.0 on error
 */
double get_disk_usage(const char *path = "/") {
    struct statvfs filesystem_stats;
    
    if (statvfs(path, &filesystem_stats) != 0) {
        return -1.0; // Error getting filesystem stats
    }

    // Calculate total and available space in bytes
    ull total_space = (ull)filesystem_stats.f_blocks * filesystem_stats.f_frsize;
    ull available_space = (ull)filesystem_stats.f_bavail * filesystem_stats.f_frsize;

    if (total_space == 0) return 0.0;

    // Calculate used space percentage
    double used_percentage = (double)(total_space - available_space) * 100.0 / (double)total_space;
    return used_percentage;
}

/**
 * Gets the system hostname
 * @return Hostname as string, or empty string on error
 */
std::string get_hostname() {
    char hostname_buffer[256];
    if (gethostname(hostname_buffer, sizeof(hostname_buffer)) == 0) {
        return std::string(hostname_buffer);
    }
    return "Unknown";
}

/**
 * Gets the current username
 * @return Username as string, or empty string on error
 */
std::string get_username() {
    struct passwd *user_info = getpwuid(getuid());
    if (user_info) {
        return std::string(user_info->pw_name);
    }
    return "Unknown";
}

/**
 * Attempts to read CPU temperature from thermal zones
 * Tries thermal_zone0 through thermal_zone9
 * @return Temperature in Celsius, or -1.0 if not available
 */
double get_cpu_temperature() {
    for (int zone = 0; zone < 10; ++zone) {
        std::string thermal_path = "/sys/class/thermal/thermal_zone" + std::to_string(zone) + "/temp";
        std::ifstream temp_file(thermal_path);
        
        if (!temp_file.is_open()) continue;

        long temperature_value = 0;
        if (temp_file >> temperature_value) {
            // Most systems report temperature in millidegrees Celsius
            if (temperature_value > 1000) {
                return temperature_value / 1000.0;
            }
            return (double)temperature_value;
        }
    }
    return -1.0; // Temperature not available
}

/**
 * Reads network interface statistics from /proc/net/dev
 * @return Map of interface name to {rx_bytes, tx_bytes}
 */
std::map<std::string, std::pair<ull, ull>> get_network_stats() {
    std::map<std::string, std::pair<ull, ull>> interface_stats;
    std::ifstream dev_file("/proc/net/dev");
    
    if (!dev_file.is_open()) {
        return interface_stats; // Return empty map on error
    }

    std::string line;
    // Skip the two header lines
    std::getline(dev_file, line);
    std::getline(dev_file, line);

    // Parse each network interface line
    while (std::getline(dev_file, line)) {
        std::istringstream line_stream(line);
        std::string interface_name;
        
        if (!(line_stream >> interface_name)) continue;

        // Remove the trailing colon from interface name
        if (!interface_name.empty() && interface_name.back() == ':') {
            interface_name.pop_back();
        }

        ull rx_bytes = 0, tx_bytes = 0;
        
        // Parse the rest of the line to get rx and tx bytes
        std::string remaining_line;
        std::getline(line_stream, remaining_line);
        std::istringstream stats_stream(remaining_line);
        
        stats_stream >> rx_bytes; // First value is rx_bytes
        
        // Skip 7 values to get to tx_bytes (9th value after interface name)
        for (int i = 0; i < 7; ++i) {
            ull temp_value;
            stats_stream >> temp_value;
        }
        
        stats_stream >> tx_bytes; // This is tx_bytes
        
        interface_stats[interface_name] = {rx_bytes, tx_bytes};
    }

    return interface_stats;
}

// =============================================================================
// UTILITY FUNCTIONS
// =============================================================================

/**
 * Converts bytes to human-readable format (B, KB, MB, GB, TB)
 * @param bytes Number of bytes to convert
 * @return Formatted string with appropriate unit
 */
std::string format_bytes(ull bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = (double)bytes;
    int unit_index = 0;

    // Convert to appropriate unit
    while (value >= 1024.0 && unit_index < 4) {
        value /= 1024.0;
        unit_index++;
    }

    std::ostringstream formatted;
    formatted << std::fixed << std::setprecision(2) << value << " " << units[unit_index];
    return formatted.str();
}

/**
 * Formats uptime seconds into human-readable format
 * @param seconds Uptime in seconds
 * @return Formatted string like "2d 5h 30m"
 */
std::string format_uptime(double seconds) {
    int total_seconds = (int)seconds;
    int days = total_seconds / 86400;
    int hours = (total_seconds % 86400) / 3600;
    int minutes = (total_seconds % 3600) / 60;

    std::ostringstream formatted;
    if (days > 0) {
        formatted << days << "d " << hours << "h " << minutes << "m";
    } else if (hours > 0) {
        formatted << hours << "h " << minutes << "m";
    } else {
        formatted << minutes << "m " << (total_seconds % 60) << "s";
    }
    
    return formatted.str();
}

// =============================================================================
// UI DRAWING FUNCTIONS
// =============================================================================

/**
 * Draws a box using Unicode box-drawing characters
 * @param y Top-left Y coordinate
 * @param x Top-left X coordinate  
 * @param height Box height
 * @param width Box width
 */
void draw_box(int y, int x, int height, int width) {
    // Draw corners
    mvprintw(y, x, "┌");                           // Top-left
    mvprintw(y, x + width - 1, "┐");               // Top-right
    mvprintw(y + height - 1, x, "└");              // Bottom-left
    mvprintw(y + height - 1, x + width - 1, "┘");  // Bottom-right

    // Draw horizontal edges
    for (int i = 1; i < width - 1; i++) {
        mvprintw(y, x + i, "─");                   // Top edge
        mvprintw(y + height - 1, x + i, "─");      // Bottom edge
    }

    // Draw vertical edges
    for (int i = 1; i < height - 1; i++) {
        mvprintw(y + i, x, "│");                   // Left edge
        mvprintw(y + i, x + width - 1, "│");       // Right edge
    }
}

/**
 * Draws a modern progress bar with Unicode block characters
 * @param row Y position for the bar
 * @param col X position for the bar
 * @param percentage Value to display (0.0-100.0)
 * @param label Text label for the bar
 */
void draw_progress_bar(int row, int col, double percentage, const char* label) {
    const int bar_width = 35;  // Width of the progress bar
    int filled_blocks = (int)(percentage / 100.0 * bar_width);

    // Ensure percentage is within valid range
    if (percentage < 0.0) percentage = 0.0;
    if (percentage > 100.0) percentage = 100.0;

    // Print label and opening bracket
    mvprintw(row, col, "%s │", label);

    // Draw the progress bar
    for (int i = 0; i < bar_width; i++) {
        if (i < filled_blocks) {
            addstr("█");  // Full block character
        } else {
            addstr(" ");  // Empty space
        }
    }

    // Print closing bracket and percentage
    printw("│ %6.2f%%", percentage);
}

// =============================================================================
// MAIN PROGRAM
// =============================================================================

int main() {
    try {
        // Initialize for UTF-8 support and prime data collection
        setlocale(LC_ALL, "");
        
        // Get initial network stats for rate calculation
        auto previous_network_stats = get_network_stats();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // Initialize ncurses
        initscr();
        if (has_colors()) {
            start_color();
            // You can add color pairs here if desired
        }
        noecho();        // Don't display typed characters
        curs_set(0);     // Hide cursor
        nodelay(stdscr, TRUE); // Make getch() non-blocking

        // Main display loop
        while (true) {
            // Check for 'q' key to quit
            int ch = getch();
            if (ch == 'q' || ch == 'Q') {
                break;
            }

            // Collect system information
            double cpu_usage = get_cpu_usage();
            double ram_usage = get_ram_usage();
            double uptime = get_uptime_seconds();
            double disk_usage = get_disk_usage("/");
            double temperature = get_cpu_temperature();
            
            std::string hostname = get_hostname();
            std::string username = get_username();

            // Calculate network transfer rates
            auto current_network_stats = get_network_stats();
            ull total_rx_rate = 0, total_tx_rate = 0;
            const double time_interval = 1.0; // seconds

            // Sum up rates from all interfaces (excluding loopback)
            for (const auto &interface : current_network_stats) {
                const std::string &interface_name = interface.first;
                if (interface_name == "lo") continue; // Skip loopback interface

                ull current_rx = interface.second.first;
                ull current_tx = interface.second.second;
                
                // Get previous values (or current if interface is new)
                ull previous_rx = 0, previous_tx = 0;
                if (previous_network_stats.count(interface_name)) {
                    previous_rx = previous_network_stats[interface_name].first;
                    previous_tx = previous_network_stats[interface_name].second;
                }

                // Calculate rate (handle counter wraparound)
                ull rx_delta = (current_rx >= previous_rx) ? (current_rx - previous_rx) : 0;
                ull tx_delta = (current_tx >= previous_tx) ? (current_tx - previous_tx) : 0;
                
                total_rx_rate += rx_delta;
                total_tx_rate += tx_delta;
            }
            
            previous_network_stats = current_network_stats;

            // Clear screen and prepare for drawing
            erase();

            // Define box dimensions
            const int box_x = 2;
            const int box_y = 1;
            const int box_width = 70;
            const int box_height = 14;

            // Draw the main container box
            draw_box(box_y, box_x, box_height, box_width);

            // Display system information inside the box
            int current_row = box_y + 1;
            
            mvprintw(current_row++, box_x + 2, "Mini System Monitor");
            mvprintw(current_row++, box_x + 2, "────────────────────────────────────────────────");
            
            mvprintw(current_row++, box_x + 2, "Host: %s", hostname.c_str());
            mvprintw(current_row++, box_x + 2, "User: %s", username.c_str());
            mvprintw(current_row++, box_x + 2, "Uptime: %s", format_uptime(uptime).c_str());

            // Display temperature if available
            if (temperature >= 0) {
                mvprintw(current_row++, box_x + 2, "Temperature: %.1f°C", temperature);
            } else {
                mvprintw(current_row++, box_x + 2, "Temperature: Not available");
            }

            // Display network transfer rates
            mvprintw(current_row++, box_x + 2, "Network: ↓ %s/s  ↑ %s/s",
                     format_bytes(total_rx_rate / (ull)time_interval).c_str(),
                     format_bytes(total_tx_rate / (ull)time_interval).c_str());

            current_row++; // Add spacing before progress bars

            // Draw progress bars for system usage
            if (cpu_usage >= 0) {
                draw_progress_bar(current_row++, box_x + 2, cpu_usage, "CPU  ");
            }
            
            if (ram_usage >= 0) {
                draw_progress_bar(current_row++, box_x + 2, ram_usage, "RAM  ");
            }
            
            if (disk_usage >= 0) {
                draw_progress_bar(current_row++, box_x + 2, disk_usage, "Disk ");
            }

            // Update the display
            refresh();

            // Wait for next update cycle
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception &e) {
        // Clean up ncurses before showing error
        endwin();
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    // Clean up ncurses
    endwin();
    std::cout << "System monitor stopped." << std::endl;
    return 0;
}
