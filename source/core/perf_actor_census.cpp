#include "perf_actor_census.h"

#include "coreactor.h"
#include "gamefuncs.h"
#include "perf_audit.h"
#include "textures/buildtiles.h"

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace
{
constexpr size_t GroupCapacity = 4096;
constexpr size_t OutputGroups = 32;

struct Group
{
    const PClass* type = nullptr;
    int stat = 0, picnum = 0, texture = 0;
    uint32_t count = 0;
};

// Fixed scratch, reused between samples. No actor pointer is retained.
std::array<Group, GroupCapacity> Groups;
std::array<const Group*, GroupCapacity> Sorted;
std::array<uint32_t, MAXSTATUS> StatusCounts;
char Output[65536];

class JsonFields
{
    size_t used = 0;
    bool valid = true;
public:
    void Append(const char* format, ...)
    {
        if (!valid) return;
        va_list args;
        va_start(args, format);
        const int length = vsnprintf(Output + used, sizeof(Output) - used, format, args);
        va_end(args);
        if (length < 0 || static_cast<size_t>(length) >= sizeof(Output) - used)
            valid = false;
        else
            used += static_cast<size_t>(length);
    }
    void ClassName(const char* name)
    {
        // Class names are short normally. Bound output even for unusual mods;
        // escaped bytes preserve valid JSON and the row reports truncation.
        Append("\"");
        size_t count = 0;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(name);
            *p && count < 192; ++p, ++count)
        {
            if (*p == '"' || *p == '\\') Append("\\%c", *p);
            else if (*p < 32 || *p >= 127) Append("\\u%04x", *p);
            else Append("%c", *p);
        }
        Append("\"");
    }
    bool Valid() const { return valid; }
};

size_t GroupHash(const PClass* type, int stat, int picnum, int texture)
{
    uint64_t hash = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(type));
    for (int value : { stat, picnum, texture })
        hash = (hash ^ static_cast<uint32_t>(value)) * 1099511628211ull;
    return static_cast<size_t>(hash) & (GroupCapacity - 1);
}
}

void PerfAuditCaptureActors()
{
    if (!PerfAuditCensusRequested()) return;
    PerfAuditScope overhead(PerfAuditMetric::AuditCensusNs);
    const uint64_t start = PerfAuditClockNs();
    const PerfAuditToken token = PerfAuditCurrentToken();
    Groups.fill({});
    StatusCounts.fill(0);
    uint32_t total = 0, exists = 0, euthanized = 0, invisible = 0;
    uint32_t invalidStatus = 0, statusMismatch = 0, invalidTexture = 0, overflowActors = 0;
    TSpriteIterator<DCoreActor> iterator;
    while (auto actor = iterator.Next())
    {
        ++total;
        exists += actor->exists();
        euthanized += (actor->ObjectFlags & OF_EuthanizeMe) != 0;
        invisible += !!(actor->spr.cstat & CSTAT_SPRITE_INVISIBLE);
        const int stat = actor->spr.statnum;
        if (static_cast<unsigned>(stat) < StatusCounts.size()) ++StatusCounts[stat];
        else ++invalidStatus;
        statusMismatch += stat != actor->link_stat;
        const int texture = actor->spr.spritetexture().GetIndex();
        invalidTexture += texture < 0;
        const int picnum = actor->spr.picnum;
        const PClass* type = actor->GetClass();
        const size_t first = GroupHash(type, stat, picnum, texture);
        bool inserted = false;
        for (size_t probe = 0; probe < GroupCapacity; ++probe)
        {
            Group& group = Groups[(first + probe) & (GroupCapacity - 1)];
            if (!group.type)
                group = { type, stat, picnum, texture, 0 };
            if (group.type == type && group.stat == stat && group.picnum == picnum && group.texture == texture)
            {
                ++group.count;
                inserted = true;
                break;
            }
        }
        if (!inserted) ++overflowActors;
    }
    size_t groupCount = 0;
    for (const auto& group : Groups)
        if (group.type) Sorted[groupCount++] = &group;
    std::sort(Sorted.begin(), Sorted.begin() + groupCount, [](const Group* a, const Group* b)
    {
        if (a->count != b->count) return a->count > b->count;
        const int nameOrder = std::strcmp(a->type->TypeName.GetChars(), b->type->TypeName.GetChars());
        if (nameOrder) return nameOrder < 0;
        if (a->stat != b->stat) return a->stat < b->stat;
        if (a->picnum != b->picnum) return a->picnum < b->picnum;
        return a->texture < b->texture;
    });
    const size_t emitted = std::min(groupCount, OutputGroups);
    uint32_t represented = 0;
    for (size_t i = 0; i < emitted; ++i) represented += Sorted[i]->count;
    const double censusMs = static_cast<double>(PerfAuditClockNs() - start) / 1000000.0;
    JsonFields json;
    json.Append("\"population\":\"linked_actor_status_lists\",\"total\":%u,\"numsprites\":%d,"
        "\"numsprites_matches\":%s,\"exists\":%u,\"euthanized\":%u,\"invisible\":%u,"
        "\"invalid_status\":%u,\"status_link_mismatches\":%u,\"invalid_texture\":%u,"
        "\"groups_complete\":%s,\"group_capacity\":%zu,\"group_count\":%zu,"
        "\"group_overflow_actors\":%u,\"top_group_count\":%zu,\"top_actors\":%u,"
        "\"remainder_actors\":%u,\"census_ms\":%.6f,\"scratch_bytes\":%zu,\"by_status\":[",
        total, Numsprites, total == static_cast<uint32_t>(Numsprites) ? "true" : "false",
        exists, euthanized, invisible, invalidStatus, statusMismatch, invalidTexture,
        overflowActors ? "false" : "true", GroupCapacity, groupCount, overflowActors, emitted,
        represented, total - represented, censusMs,
        sizeof(Groups) + sizeof(Sorted) + sizeof(StatusCounts) + sizeof(Output));
    bool firstStatus = true;
    for (size_t stat = 0; stat < StatusCounts.size(); ++stat)
        if (StatusCounts[stat])
        {
            json.Append("%s[%zu,%u]", firstStatus ? "" : ",", stat, StatusCounts[stat]);
            firstStatus = false;
        }
    json.Append("],\"top_groups\":[");
    for (size_t i = 0; i < emitted; ++i)
    {
        const Group& group = *Sorted[i];
        const char* name = group.type->TypeName.GetChars();
        json.Append("%s{\"class\":", i ? "," : "");
        json.ClassName(name);
        json.Append(",\"class_name_truncated\":%s,\"stat\":%d,\"picnum\":%d,\"texture\":%d,\"count\":%u}",
            std::strlen(name) > 192 ? "true" : "false", group.stat, group.picnum, group.texture, group.count);
    }
    json.Append("]");
    if (json.Valid()) PerfAuditWriteEvent("actor_census", token, Output);
    else PerfAuditAdd(PerfAuditMetric::AuditEventsDropped);
}
