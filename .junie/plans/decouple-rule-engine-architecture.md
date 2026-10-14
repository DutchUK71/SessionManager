---
sessionId: session-261008-121651-ixvx
---

# Requirements

### Overview & Goals
The goal of this architectural refactoring is to completely decouple the rule engine (and all its associated evaluation, temporal catalog, and schema parsing types) from the session manager and presentation layers (`DataManager`, `RuleServer`, `ScreenData`, `ScreenFile`, `Summary`, `PdfTemplateResolver`, `EmailCommandResolver`). 

The ultimate objective is to enable replacing the existing custom rule engine in its entirety with a modern, external rule engine library without requiring modifications to session management, screen navigation, HTTP routing, or presentation logic.

To make the execution transparent, manageable, and verifiable, the plan is broken down into a series of smaller, self-contained sub-plans. Each sub-plan specifies exact deliverables, interface contracts, and unambiguous verification criteria (compile gates, unit tests, and behavioral assertions) so that both the plan content and the quality of execution can be independently verified at every stage.

### Scope

#### In Scope
- **Domain State Abstraction (Sub-Plan 1)**: Introducing an engine-agnostic Session State / Working Memory interface (`ISessionState`, `IInstanceRecord`) to decouple entity/instance data representation from the rule engine's internal memory format.
- **Rule Engine Port & Adapter Architecture (Sub-Plan 1 & 2)**: Designing an engine-agnostic `IRuleEngine` interface specifying operations for rule loading, inference evaluation, condition resolution, and navigation assistance, encapsulated in a clean Service Provider Interface (SPI).
- **Legacy Rule Engine Isolation (Sub-Plan 2)**: Encapsulating the existing rule engine (`Rules/*`, `SessionModelRuleEvaluator`, `SessionModelImporter`, `SessionModelTemporal*`, `JSONFile`, `Functions`) inside `LegacyRuleEngineAdapter` and segregating it into a distinct build target (`RuleEngineLegacy`).
- **Session Layer Decoupling (Sub-Plan 3)**: Refactoring `DataManager` and `Summary` to depend exclusively on `ISessionState` and `IRuleEngine`.
- **Server & Presentation Decoupling (Sub-Plan 4)**: Refactoring `RuleServer`, `PdfTemplateResolver`, and `EmailCommandResolver` to interact strictly via generic interfaces, eliminating all rule-engine header leakage.
- **Pluggable Factory & Verification Harness (Sub-Plan 5)**: Implementing `RuleEngineFactory`, creating a reference `NewRuleEngineAdapter` skeleton, and building an automated end-to-end parity test suite.

#### Out of Scope
- Implementing the internal evaluation algorithms of the future third-party rule engine library itself (only the engine-agnostic adapter contract, factory hooks, and mock skeleton are built).
- Modifying UI JSON schemas, screen definition syntax, or client-side form layouts unless required for engine-agnostic data representation.
- Altering HTTP endpoint contracts, request/response formats, or client-facing REST APIs exposed by `RuleServer`.

### User Stories
- **As a Developer**, I want the session manager and presentation layers to interact with the rule engine via an engine-agnostic interface so that I can replace or upgrade the rule engine implementation without rewriting UI, routing, or persistence code.
- **As a System Architect**, I want session state to be independent of rule engine internals so that data can be serialized, validated, or modified without tightly binding to concrete rule structures.
- **As a Reviewer / Verifier**, I want each step of the refactoring to be broken into smaller, independently testable milestones with concrete verification criteria so that I can easily inspect progress and confirm behavioral equivalence at each stage.
- **As a Maintainer**, I want clear module boundaries enforced at the build system level so that compiler dependencies are minimized and circular couplings are permanently prevented.

### Functional Requirements
1. **Model Loading & Compilation**: `IRuleEngine` must support initializing and compiling rule models from raw definitions (e.g., JSON schema, AST, or external engine formats) and returning structured diagnostics.
2. **Session Evaluation & Inference**: The engine interface must accept changes to attributes/instances (`AttributeMutation`) and return evaluation outcomes (`RuleEvaluationResult` containing inferred values, active validations, updated state, and evaluation diagnostics).
3. **Navigation & Flow Decisions**: The engine interface must provide screen redirection or flow evaluation (`ResolveNextScreen`) without embedding presentation concepts directly inside rule objects.
4. **Data Isolation**: Presentation consumers (`DataManager`, `PdfTemplateResolver`, `EmailCommandResolver`, `Summary`) must retrieve attribute values and instance hierarchies via `ISessionState` without direct knowledge of `Rules::Entity`, `Rules::Instance`, or `Rules::RuleSet`.
5. **Pluggable Engine Selection**: The application must support selecting the active rule engine backend via configuration and `RuleEngineFactory`.

### Non-Functional Requirements & Verification Goals
- **Strict Header Separation**: Zero direct `#include` references to `Rules/` or `SessionModel*` outside the `Adapters/Legacy/` directory.
- **Compile-Time Target Isolation**: Distinct CMake library targets (`SessionCore`, `SessionLogic`, `RuleEngineLegacy`, `SessionServer`) enforcing boundary integrity at compile time.
- **100% Behavioral Parity**: All existing HTTP API endpoints, screen flows, instance mutations, and PDF/Email resolutions must produce identical results when executed via `LegacyRuleEngineAdapter`.
- **Zero Memory / Resource Leaks**: All generic interfaces must use clear lifecycle ownership (`std::unique_ptr`, `std::shared_ptr`, and `std::string_view` / const references).

# Technical Design

### Current Implementation & Coupling Map

Currently, the session manager and rule engine are tightly coupled across multiple layers:
1. **Shared Data Structure Dual-Roles**:
   - `Rules/Entity.h`, `Rules/Instance.h`, and `Rules/Attribute.h` serve simultaneously as the working memory for rule evaluation AND the general session data storage for `DataManager` and `RuleServer`.
   - `DataManager::HandleData`, `GetSessionData`, `GetPageData`, and `UpdateAttributeValue` take `shared_ptr<vector<shared_ptr<Entity>>>` and `shared_ptr<RuleSet>&` directly.
2. **Direct Execution & Evaluation Calls**:
   - `RuleServer.cpp` directly instantiates `SessionModelRuleEvaluator`, configures `Functions`, and parses diagnostics from `SessionModelImporter`.
   - `DataManager.cpp` queries `ruleSet->uploads` and calls `ruleSet->GetLatestScreenLocation(directTo)` for screen redirection.
3. **Presentation & Template Generation Coupling**:
   - `PdfTemplateResolver.cpp` and `EmailCommandResolver.cpp` directly traverse `Entity`, `Instance`, `Relationship`, and `RuleSet` objects to resolve placeholder tokens.
   - `Summary.cpp` serializes entity instances by reading directly from `Rules/Instance.h`.

---

### Key Decisions

1. **Hexagonal Architecture (Ports and Adapters)**:
   - *Decision*: Introduce explicit core ports `IRuleEngine` and `ISessionState`.
   - *Rationale*: Isolates the session manager from rule evaluation details and allows any replacement engine to plug in seamlessly via an adapter.

2. **Decouple Session Working Memory from Rule Internals**:
   - *Decision*: Introduce an engine-neutral session state model (`SessionState` implementing `ISessionState`) that represents entities, instances, attributes, and relationships.
   - *Rationale*: A new rule engine library may use its own internal working memory (e.g., Rete graph, AST nodes, or relational tuples). Decoupling the session data model ensures the session layer does not need to adapt to internal memory representations.

3. **Separate Presentation Concerns from Rule Engine Data**:
   - *Decision*: Move screen redirection history and file upload metadata out of `RuleSet` and into session navigation context or explicit result DTOs (`NavigationDecision`).
   - *Rationale*: Rule engines should focus on logic inference and decision evaluation, not UI upload tracking or screen history.

4. **Multi-Target Modular CMake Build**:
   - *Decision*: Restructure `CMakeLists.txt` into `SessionEngineCore` (interfaces & generic state), `RuleEngineLegacy` (isolated current engine), `SessionEngineLogic` (DataManager & presentation), and the executable application.
   - *Rationale*: Enforces architectural boundaries at compile time by preventing unauthorized header inclusions.

---

### Architecture Diagram

``` mermaid
graph TD
    subgraph Presentation & Server Layer
        RS[RuleServer / HTTP Endpoints] --> DM[DataManager]
        RS --> PTR[PdfTemplateResolver]
        RS --> ECR[EmailCommandResolver]
        DM --> SS[ISessionState]
        PTR --> SS
        ECR --> SS
    end

    subgraph Core Abstraction Layer [Ports & Contracts]
        DM --> IRE[IRuleEngine]
        REF[RuleEngineFactory] --> IRE
        IRE --> RC[RuleEvaluationContext]
        IRE --> RR[RuleEvaluationResult]
        SS --> IR[IInstanceRecord]
    end

    subgraph Adapter Layer
        IRE <|-- LRA[LegacyRuleEngineAdapter]
        IRE <|-- NRA[NewRuleEngineAdapter]
    end

    subgraph Concrete Rule Engines
        LRA --> ORE[Legacy Rule Engine: RuleSet / Evaluator / Functions / SessionModel*]
        NRA --> NRE[External Rule Engine Library]
    end
```

---

### Data Models & Contracts

#### 1. Core Rule Engine Interface (`Core/IRuleEngine.h`)
```cpp
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "SessionStateContracts.h"
#include "ISessionState.h"

namespace SessionEngine
{
    struct AttributeMutation
    {
        std::string entityId;
        std::string instanceId;
        std::string attributeId;
        std::string rawValue;
    };

    struct EvaluationDiagnostic
    {
        enum class Severity { Info, Warning, Error };
        Severity severity;
        std::string code;
        std::string message;
        std::string location;
    };

    struct ValidationEvent
    {
        std::string entityId;
        std::string instanceId;
        std::string ruleId;
        std::string message;
        bool isBlocking = false;
    };

    struct RuleEvaluationResult
    {
        bool success = true;
        std::vector<AttributeMutation> inferredChanges;
        std::vector<ValidationEvent> validationEvents;
        std::vector<EvaluationDiagnostic> diagnostics;
        std::optional<std::string> targetScreenId;
    };

    class IRuleEngine
    {
    public:
        virtual ~IRuleEngine() = default;

        // Lifecycle & Model Compilation
        virtual bool LoadModelFromJson(const std::string& modelJson, std::vector<EvaluationDiagnostic>& outDiagnostics) = 0;
        virtual bool IsModelLoaded() const = 0;

        // Inference & Evaluation
        virtual RuleEvaluationResult Evaluate(ISessionState& sessionState, const std::vector<AttributeMutation>& changes) = 0;
        virtual RuleEvaluationResult EvaluateAll(ISessionState& sessionState) = 0;

        // Navigation & Flow Resolution
        virtual std::optional<std::string> ResolveNextScreen(const ISessionState& sessionState, const std::string& directToScreen) = 0;
    };
}
```

#### 2. Generic Session State Interface (`Core/ISessionState.h`)
```cpp
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace SessionEngine
{
    class IInstanceRecord
    {
    public:
        virtual ~IInstanceRecord() = default;
        virtual std::string GetInstanceId() const = 0;
        virtual std::string GetEntityId() const = 0;
        virtual std::optional<std::string> GetAttributeValue(const std::string& attributeId) const = 0;
        virtual void SetAttributeValue(const std::string& attributeId, const std::string& value) = 0;
        virtual std::unordered_map<std::string, std::string> GetAllAttributes() const = 0;
        virtual std::vector<std::string> GetLinkedInstanceIds(const std::string& relationshipId) const = 0;
        virtual void AddRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId) = 0;
        virtual void RemoveRelationshipLink(const std::string& relationshipId, const std::string& targetInstanceId) = 0;
    };

    class ISessionState
    {
    public:
        virtual ~ISessionState() = default;
        virtual std::shared_ptr<IInstanceRecord> GetInstance(const std::string& entityId, const std::string& instanceId) = 0;
        virtual std::shared_ptr<IInstanceRecord> CreateInstance(const std::string& entityId, const std::string& parentInstanceId = "", const std::string& relationshipId = "") = 0;
        virtual bool DeleteInstance(const std::string& entityId, const std::string& instanceId) = 0;
        virtual std::vector<std::shared_ptr<IInstanceRecord>> GetAllInstances(const std::string& entityId) const = 0;
        virtual void CopyInstanceData(const std::string& sourceInstanceId, const std::string& targetInstanceId) = 0;
        virtual std::string SerializeToJson() const = 0;
        virtual bool DeserializeFromJson(const std::string& jsonString) = 0;
        virtual void Clear() = 0;
    };
}
```

#### 3. Rule Engine Factory (`Core/RuleEngineFactory.h`)
```cpp
#pragma once
#include <memory>
#include <string>
#include "IRuleEngine.h"

namespace SessionEngine
{
    enum class EngineType
    {
        Legacy,
        ExternalLibrary,
        Mock
    };

    class RuleEngineFactory
    {
    public:
        static std::unique_ptr<IRuleEngine> CreateEngine(EngineType type = EngineType::Legacy);
    };
}
```

---

### File Structure & Target Reorganization

```
SessionManager/
├── CMakeLists.txt                          (Multi-target modular CMake configuration)
├── Core/                                   (Library Target: SessionCore)
│   ├── IRuleEngine.h                       (Rule Engine Port Interface)
│   ├── ISessionState.h                     (Session Working Memory Interface)
│   ├── SessionStateContracts.h             (Generic DTOs: Mutation, Diagnostics, Events)
│   ├── SessionState.h / .cpp               (Standard in-memory ISessionState implementation)
│   └── RuleEngineFactory.h / .cpp          (Factory for selecting engine backend)
├── Session/                                (Library Target: SessionLogic)
│   ├── DataManager.h / .cpp                (Refactored to use IRuleEngine & ISessionState)
│   ├── ScreenData.h / .cpp                 (Presentation screen data)
│   ├── ScreenFile.h / .cpp                 (Screen definition parser)
│   └── Summary.h / .cpp                    (Summary serialization using ISessionState)
├── Server/                                 (Executable Target: SessionManager)
│   ├── RuleServer.h / .cpp                 (HTTP API layer interacting via IRuleEngine)
│   ├── PdfTemplateResolver.h / .cpp        (Resolves PDF fields from ISessionState)
│   ├── EmailCommandResolver.h / .cpp       (Resolves Email commands from ISessionState)
│   └── Main.cpp
└── Adapters/
    ├── Legacy/                             (Library Target: RuleEngineLegacy)
    │   ├── LegacyRuleEngineAdapter.h / .cpp(Adapts IRuleEngine to existing RuleSet/Evaluator)
    │   └── Rules/                          (Internal legacy rule engine implementation)
    │       ├── Attribute.*, Condition.*, Date.*, Entity.*, Instance.*, Relationship.*, Rule.*, RuleSet.*
    │       ├── Functions.*, JSONFile.*, SessionModel*
    └── External/
        └── NewRuleEngineAdapter.h / .cpp   (Pluggable adapter skeleton for external rule engine)
```

---

### Risks & Mitigations

| Risk | Impact | Mitigation |
| :--- | :--- | :--- |
| **Data model semantic mismatch**: New rule engine might require a specific data graph or strongly-typed ontology different from key-value attributes. | Medium | Design `ISessionState` and `IInstanceRecord` around flexible entity-attribute-value (EAV) and link graphs, allowing custom adapter transformations. |
| **Loss of evaluation diagnostics or tracing**: Legacy debug endpoints rely on fine-grained `SessionModelRuleEvaluator::EvaluationStats`. | Low | Capture diagnostics and tracing in structured `RuleEvaluationResult` DTOs that can be optionally populated by any adapter supporting tracing. |
| **Performance overhead of abstraction**: Multiple virtual calls and DTO conversions on hot evaluation paths. | Low | Pass references and use batch mutation arrays (`std::vector<AttributeMutation>`) to minimize per-attribute conversion overhead. |

# Detailed Verification & Execution Plan

To allow easy inspection and verification of both the plan content and the execution quality at every stage, the implementation is decomposed into 5 progressive sub-plans. Each sub-plan defines explicit **Deliverables**, **Implementation Details**, **Verification Checkpoints**, and a **Completion Gate**.

---

### Sub-Plan 1: Define Generic Session State & Engine Contracts
**Goal**: Establish pure engine-agnostic abstractions and working memory representations with zero external dependencies.

#### Deliverables:
- `Core/ISessionState.h` and `Core/SessionStateContracts.h`: Defines `ISessionState`, `IInstanceRecord`, `AttributeMutation`, `EvaluationDiagnostic`, `ValidationEvent`, and `RuleEvaluationResult`.
- `Core/SessionState.h` and `Core/SessionState.cpp`: Production-ready generic session state implementation.
- `Core/IRuleEngine.h`: Pure abstract SPI for rule evaluation, model compilation, and navigation.
- `Tests/SessionStateTests.cpp`: Unit test suite for session state operations.

#### Verification Checkpoints:
1. **Compilation Check**: `Core/` builds cleanly into a static library `SessionCore` without any includes of `Rules/` or `ScreenFile.h`.
2. **State CRUD Test**: Verify creating, querying, updating, and deleting entity instances across parent-child relationships via `ISessionState`.
3. **Serialization Test**: Verify serializing an active `SessionState` to JSON and deserializing it back produces an identical in-memory state.
4. **Completion Gate**: `SessionCore` builds and 100% of `SessionStateTests` pass before proceeding to Sub-Plan 2.

---

### Sub-Plan 2: Implement Legacy Adapter & Isolate Legacy Rule Engine
**Goal**: Encapsulate all existing rule engine types (`Rules/*`, `SessionModel*`, `Functions`, `JSONFile`) into an isolated `RuleEngineLegacy` library behind `LegacyRuleEngineAdapter`.

#### Deliverables:
- `Adapters/Legacy/LegacyRuleEngineAdapter.h` and `LegacyRuleEngineAdapter.cpp`: Implements `IRuleEngine` by translating generic calls to/from `RuleSet`, `SessionModelRuleEvaluator`, and `SessionModelImporter`.
- Relocated legacy files into `Adapters/Legacy/Rules/`.
- Updated `CMakeLists.txt` creating `RuleEngineLegacy` target.
- `Tests/LegacyAdapterTests.cpp`: Verification tests for adapter model loading, evaluation, and navigation.

#### Verification Checkpoints:
1. **Target Isolation Check**: Verify that `RuleEngineLegacy` compiles as an independent static library. No headers inside `Adapters/Legacy/Rules/` are exported to the public include path.
2. **Model Loading Parity**: Load a sample session model JSON through `LegacyRuleEngineAdapter::LoadModelFromJson` and verify diagnostics match legacy importer output.
3. **Evaluation Mapping Check**: Pass an `ISessionState` and `AttributeMutation` list to `LegacyRuleEngineAdapter::Evaluate`; verify inferred attributes and validation events are correctly computed by `SessionModelRuleEvaluator` and converted to `RuleEvaluationResult`.
4. **Navigation Check**: Verify `LegacyRuleEngineAdapter::ResolveNextScreen` returns the exact screen location computed by legacy `ruleSet->GetLatestScreenLocation`.
5. **Completion Gate**: `RuleEngineLegacy` builds with zero warnings, and `LegacyAdapterTests` pass with full evaluation coverage.

---

### Sub-Plan 3: Decouple DataManager & Summary Presentation Layer
**Goal**: Refactor `DataManager` and `Summary` to use `ISessionState` and `IRuleEngine`, removing all direct rule engine couplings from session logic.

#### Deliverables:
- Refactored `Session/DataManager.h` and `DataManager.cpp`: Replace `vector<shared_ptr<Entity>>` with `ISessionState&`, invoke `IRuleEngine::Evaluate` on screen submissions, and use `IRuleEngine::ResolveNextScreen` for flow transitions.
- Refactored `Session/Summary.h` and `Summary.cpp`: Populate summary structures directly from `ISessionState`.
- `Tests/DataManagerTests.cpp`: Unit tests validating screen submissions, radio selection clears, instance pagination, and summary generation.

#### Verification Checkpoints:
1. **Header Cleanliness Check**: Verify `DataManager.h` and `Summary.h` contain zero `#include "Rules/*"` or `#include "SessionModel*"`.
2. **Form Submission Test**: Verify `DataManager::HandleData` accepts input screen data, updates `ISessionState`, triggers `IRuleEngine::Evaluate`, and returns the expected screen.
3. **Summary Parity Test**: Compare `Summary::GetSummaryData` output generated via `ISessionState` against pre-refactor legacy output to ensure 100% JSON equivalence.
4. **Completion Gate**: `SessionLogic` target compiles cleanly and all `DataManagerTests` pass.

---

### Sub-Plan 4: Decouple Server Endpoints & Document Resolvers
**Goal**: Refactor `RuleServer`, `PdfTemplateResolver`, and `EmailCommandResolver` to interact strictly via `ISessionState` and `IRuleEngine`.

#### Deliverables:
- Refactored `Server/PdfTemplateResolver.h` / `.cpp`: Accepts `const ISessionState&` instead of `const RuleSet&`.
- Refactored `Server/EmailCommandResolver.h` / `.cpp`: Accepts `ISessionState&` and `IRuleEngine&` instead of `RuleSet&`.
- Refactored `Server/RuleServer.h` / `.cpp`: Server endpoints (`/submit-screen`, `/update`, `/create-instance`, `/pdf/resolve`, `/debug/session/*`) operate via `IRuleEngine` and `ISessionState`.
- Updated `CMakeLists.txt` with final executable target linking only to abstract ports and adapter libraries.

#### Verification Checkpoints:
1. **Zero Header Leakage Verification**: Static verification (e.g. grep check) ensuring no file in `Server/` or `Session/` includes any file from `Adapters/Legacy/Rules/`.
2. **PDF Token Resolution Test**: Verify `PdfTemplateResolver::Resolve` produces identical document payloads when reading fields from `ISessionState`.
3. **Email Command Resolution Test**: Verify `EmailCommandResolver::Resolve` correctly formats commands without direct `RuleSet` access.
4. **Endpoint Parity Test**: Execute HTTP requests against `RuleServer` endpoints and assert identical response codes, headers, and JSON bodies.
5. **Completion Gate**: Entire application compiles and passes full regression suite with zero legacy header leaks.

---

### Sub-Plan 5: Implement RuleEngineFactory, Pluggable Mock Adapter & Parity Verification
**Goal**: Provide runtime engine pluggability via `RuleEngineFactory`, provide a reference `NewRuleEngineAdapter` skeleton ready for the replacement library, and validate overall system swappability.

#### Deliverables:
- `Core/RuleEngineFactory.h` and `RuleEngineFactory.cpp`: Factory supporting configuration-driven engine instantiation (`Legacy`, `ExternalLibrary`, `Mock`).
- `Adapters/External/NewRuleEngineAdapter.h` and `NewRuleEngineAdapter.cpp`: Clean adapter skeleton demonstrating integration of an external rule engine library.
- `Adapters/Mock/MockRuleEngineAdapter.h` / `.cpp`: Lightweight mock engine for testing session logic in complete isolation from any real rule engine.
- `Tests/EndToEndParityTests.cpp`: Automated session test suite replaying user workflows.

#### Verification Checkpoints:
1. **Pluggability Check**: Verify switching `EngineType` in `RuleServer.config.json` between `Legacy` and `Mock` instantiates the corresponding adapter cleanly.
2. **Mock Isolation Test**: Verify `RuleServer` and `DataManager` run successfully with `MockRuleEngineAdapter` when `RuleEngineLegacy` is completely excluded from the build.
3. **End-to-End Workflow Parity**: Run full end-to-end multi-screen form submission, instance creation, and PDF export workflows, verifying 100% parity against baseline fixtures.
4. **Completion Gate**: Full end-to-end regression passes; architectural decoupling is complete and ready for the external rule engine library.

# Testing

### Validation Approach
Verification of the decoupling will be conducted in a three-stage approach:
1. **Module Boundary & Header Cleanliness**: Static verification that all session, presentation, and server translation units have zero compile-time dependencies on legacy rule engine internal headers.
2. **Behavioral Equivalence with Legacy Adapter**: Verify that running the application with `LegacyRuleEngineAdapter` produces 100% identical outputs for all existing screen workflows, HTTP endpoints, PDF generation, and rule evaluations.
3. **Adapter Swappability & Isolation**: Verify that the Session Manager builds and tests cleanly with a `MockRuleEngineAdapter` without linking `RuleEngineLegacy`.

### Key Scenarios

#### 1. Screen Submission & Rule Re-evaluation
- **Action**: Submit form inputs via `RuleServer::HandleSubmitScreenRequest` to `DataManager::HandleData`.
- **Expected Outcome**: `DataManager` updates `ISessionState`, sends mutations to `IRuleEngine::Evaluate`, applies inferred attribute results back to `ISessionState`, and navigates to the expected next screen.

#### 2. Instance Lifecycle Management
- **Action**: Create, populate, and delete multi-instance entities (e.g. household members, vehicles) via `RuleServer::HandleCreateInstanceRequest` and `HandleDeleteInstanceRequest`.
- **Expected Outcome**: `ISessionState` manages instance IDs and links; `IRuleEngine` detects instance additions/deletions and executes corresponding rules.

#### 3. PDF & Email Command Token Resolution
- **Action**: Trigger PDF resolution via `RuleServer::HandlePdfResolveRequest`.
- **Expected Outcome**: `PdfTemplateResolver` resolves entity attributes and repeat blocks through `ISessionState` queries without accessing `RuleSet` directly.

#### 4. Diagnostic & Debug Endpoints
- **Action**: Invoke `/debug/session/evaluate` and `/debug/session-model/validate`.
- **Expected Outcome**: `IRuleEngine::Evaluate` returns structured `EvaluationDiagnostic` and `RuleEvaluationResult` objects serialized into identical JSON response payloads.

### Edge Cases & Regression Verification
- **Empty / Null Session State**: Validating that rule evaluation gracefully handles initial unpopulated sessions.
- **Circular Rule Dependencies**: Ensuring adapter error reporting cleanly surfaces engine cycle detections through `EvaluationDiagnostic`.
- **Clean Build Boundary Verification**: Automated build test ensuring that removing the `RuleEngineLegacy` target still allows compiling `SessionCore`, `SessionLogic`, and `RuleServer` with the mock adapter.

# Delivery Steps

### ✓ Step 1: Define Generic Session State & Engine Contracts (Sub-Plan 1)
Core interfaces (`ISessionState`, `IInstanceRecord`, `IRuleEngine`, `RuleEvaluationResult`, etc.) and a generic `SessionState` implementation are defined in `Core/` without any rule-engine or UI dependencies, verified by isolated unit tests.

- Define generic data graph abstractions (`ISessionState`, `IInstanceRecord`, `SessionStateContracts.h`) independent of `Rules/` and `ScreenFile.h`.
- Implement `Core/SessionState.cpp` providing thread-safe instance CRUD, attribute storage, relationship link management, and JSON serialization/deserialization.
- Define `Core/IRuleEngine.h` specifying lifecycle (`LoadModelFromJson`, `IsModelLoaded`), inference (`Evaluate`, `EvaluateAll`), and navigation (`ResolveNextScreen`).
- Add `Tests/SessionStateTests.cpp` to verify instance creation, attribute mutations, hierarchy traversal, and JSON serialization.

### ✓ Step 2: Build Legacy Rule Engine Adapter and Isolate Rule Engine Build Target (Sub-Plan 2)
Existing rule engine types (`Rules/*`, `SessionModel*`, `Functions`, `JSONFile`) are moved into an isolated library target (`RuleEngineLegacy`) and adapted to `IRuleEngine` via `LegacyRuleEngineAdapter`.

- Create `Adapters/Legacy/LegacyRuleEngineAdapter.h` and `.cpp` implementing `IRuleEngine` by translating generic DTOs to/from existing `RuleSet`, `SessionModelRuleEvaluator`, and `Functions`.
- Move existing legacy rule types (`Rules/*`, `SessionModel*`, `JSONFile.*`, `Functions.*`) into `Adapters/Legacy/Rules/` behind an internal implementation boundary.
- Update `CMakeLists.txt` to isolate the legacy rule engine into a distinct static library target (`RuleEngineLegacy`), preventing header exposure to the session manager.
- Implement `Tests/LegacyAdapterTests.cpp` verifying model compilation, attribute evaluation, diagnostic propagation, and screen flow navigation through `IRuleEngine`.

### ✓ Step 3: Decouple DataManager and Summary Presentation from Rule Engine Types (Sub-Plan 3)
`DataManager` and `Summary` operate exclusively through `ISessionState` and `IRuleEngine`, with zero direct references to `Rules::Entity`, `Rules::Instance`, `Rules::Attribute`, or `Rules::RuleSet`.

- Refactor `DataManager.h` and `DataManager.cpp` to operate on `ISessionState&` and execute inference via `IRuleEngine::Evaluate`.
- Replace legacy `ruleSet->GetLatestScreenLocation(directTo)` in `DataManager` with `IRuleEngine::ResolveNextScreen`.
- Refactor `Summary.h` and `Summary.cpp` to construct summary groups and entity trees from `ISessionState` instead of inspecting `Rules/Instance.h`.
- Add `Tests/DataManagerTests.cpp` validating screen submissions, radio selection clears, and summary JSON output equivalence.

### ✓ Step 4: Decouple Server Endpoints and Template Resolvers (Sub-Plan 4)
`RuleServer`, `PdfTemplateResolver`, and `EmailCommandResolver` interact strictly with `ISessionState` and `IRuleEngine`, eliminating all remaining rule-engine header inclusions from `Server/`.

- Refactor `PdfTemplateResolver.h` and `.cpp` to resolve placeholders and repeat blocks from `const ISessionState&` rather than `const RuleSet&`.
- Refactor `EmailCommandResolver.h` and `.cpp` to resolve command definitions and bindings through `ISessionState&` and `IRuleEngine&`.
- Refactor `RuleServer.h` and `RuleServer.cpp` endpoint handlers (`/submit-screen`, `/update`, `/create-instance`, `/pdf/resolve`, `/debug/session/*`) to use `IRuleEngine` and `ISessionState`.
- Remove all `#include "Rules/*"` and `#include "SessionModel*"` from `RuleServer.h`, `DataManager.h`, `PdfTemplateResolver.h`, and `EmailCommandResolver.h`.

### ✓ Step 5: Implement Pluggable Factory, Mock Adapter Skeleton, and End-to-End Parity Suite (Sub-Plan 5)
Dynamic engine instantiation is enabled via `RuleEngineFactory`, a verifiable mock/skeleton adapter is provided for the future rule engine library, and full behavioral parity is validated.

- Implement `Core/RuleEngineFactory.h` and `.cpp` enabling runtime engine selection (`Legacy`, `ExternalLibrary`, `Mock`) based on configuration.
- Implement `Adapters/External/NewRuleEngineAdapter.h` and `.cpp` as a clean adapter skeleton ready for the future rule engine library.
- Implement `Adapters/Mock/MockRuleEngineAdapter.h` and `.cpp` enabling standalone execution and testing of the session layer without the legacy rule engine.
- Create automated end-to-end regression tests verifying that full form submissions, screen transitions, and PDF exports produce 100% parity against baseline fixtures.