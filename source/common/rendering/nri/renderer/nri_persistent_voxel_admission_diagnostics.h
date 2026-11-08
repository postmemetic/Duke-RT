#pragma once

#include <cstdint>
#include <memory>
#include <vector>

class NRIPersistentVoxelResidency;
struct NRIPersistentVoxelAdmissionDiagnosticState;

// Correctness capture only. Disabled scopes never inspect the queue or allocate.
// Observed scheduler stages are CPU bookkeeping, not GPU completion timestamps.
class NRIVoxelAdmissionDiagnosticScope
{
public:
    NRIVoxelAdmissionDiagnosticScope(NRIPersistentVoxelResidency& owner, uint32_t frame, const char* phase);
    ~NRIVoxelAdmissionDiagnosticScope();
    bool Active() const { return state != nullptr; }
    void BeginBudget();
    double ElapsedMs(double rawElapsedMs) const;
    void Budgets(uint32_t variants, uint64_t bytes, uint32_t blas, double ms);
    void Candidate(uint64_t pair, uint32_t position);
    void Visit(uint64_t pair);
    void Transition(uint64_t pair, const char* event);
    void Attempt();
    void Disposition(const char* value) { disposition = value; }
    void EndVisit();
    void Finish(const char* stop, uint32_t admitted, uint64_t bytes, uint32_t blas);
private:
    NRIPersistentVoxelResidency& owner;
    // Audit I/O can synchronously disable auditing; later callbacks may clear
    // the owner history while this pump still unwinds. Keep its lifetime pinned.
    std::shared_ptr<NRIPersistentVoxelAdmissionDiagnosticState> stateOwner;
    NRIPersistentVoxelAdmissionDiagnosticState* state = nullptr;
    uint32_t frame;
    const char* phase;
    uint64_t pump = 0, currentPair = 0, lastVisitedPair = 0;
    uint64_t byteBudget = 0, bytesUsed = 0;
    uint64_t budgetObserverStart = 0, budgetWallStart = 0, budgetRawNs = 0, budgetExcludedNs = 0;
    uint32_t variantBudget = 0, blasBudget = 0, admitted = 0, blasUsed = 0, visited = 0, attempted = 0;
    double msBudget = 0;
    const char* disposition = "visited", *lastDisposition = "none", *stop = "early-return";
    std::vector<uint64_t> candidates;
};

class NRIVoxelAdmissionEntryDiagnosticScope
{
public:
    NRIVoxelAdmissionEntryDiagnosticScope(NRIVoxelAdmissionDiagnosticScope& owner, uint64_t pair) : owner(owner) { owner.Visit(pair); }
    ~NRIVoxelAdmissionEntryDiagnosticScope() { owner.EndVisit(); }
private:
    NRIVoxelAdmissionDiagnosticScope& owner;
};

void NoteNRIVoxelAdmissionDiagnostic(NRIPersistentVoxelResidency& owner, uint64_t pair,
    const char* event, int32_t result = -1);
// Called at existing compute mutations; does not query, poll, cancel, or own jobs.
void NoteNRIVoxelAdmissionComputeDiagnostic(uint64_t mesh, uint64_t generation,
    const char* event, uint32_t job, uint64_t rendererFrame, uint64_t fence = 0);

void NoteNRIVoxelAdmissionRejectedDiagnostic(uint64_t mesh, uint64_t material,
    uint32_t map, const char* reason);
