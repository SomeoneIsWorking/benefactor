#include "port/guest_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

namespace benefactor::diag {
namespace {

/* One owner's hot PCs. A fixed open-addressed table: no allocation, no lock,
 * and a sample costs one hash and one compare in the common case. Overflow is
 * deliberately dropped rather than grown — the question is always "what is the
 * hot loop", and a loop that matters is in the table long before it fills. */
class HotProgramCounters {
  public:
    static constexpr std::size_t kSlots = 512;

    void sample(std::uint32_t program_counter) {
        total_.fetch_add(1, std::memory_order_relaxed);
        std::size_t slot = hash(program_counter);
        for (std::size_t probe = 0; probe < kSlots; ++probe) {
            const std::size_t at = (slot + probe) % kSlots;
            std::uint32_t held = pc_[at].load(std::memory_order_relaxed);
            if (held == program_counter) {
                count_[at].fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (held == kEmpty) {
                pc_[at].store(program_counter, std::memory_order_relaxed);
                count_[at].fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }
        dropped_.fetch_add(1, std::memory_order_relaxed);
    }

    void reset() {
        for (std::size_t at = 0; at < kSlots; ++at) {
            pc_[at].store(kEmpty, std::memory_order_relaxed);
            count_[at].store(0, std::memory_order_relaxed);
        }
        total_.store(0, std::memory_order_relaxed);
        dropped_.store(0, std::memory_order_relaxed);
    }

    struct Entry {
        std::uint32_t program_counter;
        std::uint64_t count;
    };

    /* The `wanted` hottest PCs, hottest first. */
    std::size_t hottest(Entry *into, std::size_t wanted) const {
        std::array<Entry, kSlots> all{};
        std::size_t found = 0;
        for (std::size_t at = 0; at < kSlots; ++at) {
            const std::uint32_t held = pc_[at].load(std::memory_order_relaxed);
            if (held == kEmpty)
                continue;
            all[found++] = Entry{held, count_[at].load(std::memory_order_relaxed)};
        }
        std::sort(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(found),
                  [](const Entry &left, const Entry &right) { return left.count > right.count; });
        const std::size_t take = std::min(wanted, found);
        std::copy(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(take), into);
        return take;
    }

    std::uint64_t total() const { return total_.load(std::memory_order_relaxed); }

  private:
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

    static std::size_t hash(std::uint32_t program_counter) {
        /* PCs are even and cluster tightly, so mix before masking. */
        std::uint32_t mixed = program_counter * 2654435761u;
        return (mixed >> 13) % kSlots;
    }

    std::array<std::atomic<std::uint32_t>, kSlots> pc_{};
    std::array<std::atomic<std::uint64_t>, kSlots> count_{};
    std::atomic<std::uint64_t> total_{0};
    std::atomic<std::uint64_t> dropped_{0};
};

/* Frame-accounting owners, in the order they are reported. */
struct Owner {
    std::uint32_t id;
    const char *name;
};
constexpr std::array<Owner, 3> kOwners{{{0u, "flow"}, {3u, "irq3"}, {6u, "irq6"}}};

class GuestProfile {
  public:
    static GuestProfile &instance() {
        static GuestProfile only;
        return only;
    }

    GuestProfile() {
        for (auto &table : tables_)
            table.reset();
    }

    void sample(std::uint32_t program_counter, std::uint32_t owner) {
        HotProgramCounters *table = table_for(owner);
        if (table != nullptr)
            table->sample(program_counter);
    }

    void reset() {
        for (auto &table : tables_)
            table.reset();
    }

    int report(char *text, int capacity) const {
        if (text == nullptr || capacity <= 0)
            return 0;
        int written = 0;
        for (std::size_t index = 0; index < kOwners.size(); ++index) {
            const HotProgramCounters &table = tables_[index];
            const std::uint64_t total = table.total();
            if (total == 0)
                continue;
            written += std::snprintf(text + written, static_cast<std::size_t>(capacity - written),
                                     "%s: %llu chip accesses", kOwners[index].name,
                                     static_cast<unsigned long long>(total));
            std::array<HotProgramCounters::Entry, kTop> top{};
            const std::size_t found = table.hottest(top.data(), kTop);
            for (std::size_t at = 0; at < found && written < capacity; ++at) {
                const double share =
                    100.0 * static_cast<double>(top[at].count) / static_cast<double>(total);
                written +=
                    std::snprintf(text + written, static_cast<std::size_t>(capacity - written),
                                  " | $%06X %.0f%%", top[at].program_counter, share);
            }
            if (written < capacity)
                written += std::snprintf(text + written,
                                         static_cast<std::size_t>(capacity - written), "\n");
        }
        return written;
    }

  private:
    static constexpr std::size_t kTop = 6;

    HotProgramCounters *table_for(std::uint32_t owner) {
        for (std::size_t index = 0; index < kOwners.size(); ++index)
            if (kOwners[index].id == owner)
                return &tables_[index];
        return nullptr;
    }

    std::array<HotProgramCounters, kOwners.size()> tables_{};
};

} // namespace
} // namespace benefactor::diag

extern "C" {

void pc_profile_sample(uint32_t program_counter, uint32_t owner) {
    benefactor::diag::GuestProfile::instance().sample(program_counter, owner);
}

int pc_profile_report(char *text, int capacity) {
    return benefactor::diag::GuestProfile::instance().report(text, capacity);
}

void pc_profile_reset(void) { benefactor::diag::GuestProfile::instance().reset(); }
}
