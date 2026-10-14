#include "Core/SessionState.h"
#include <cassert>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

void TestInstanceCRUD()
{
    SessionEngine::SessionState state;

    // Create instances
    auto person1 = state.CreateInstance("Person");
    assert(person1 != nullptr);
    assert(person1->GetEntityId() == "Person");
    assert(!person1->GetInstanceId().empty());

    // Attributes
    person1->SetAttributeValue("firstName", "Alice");
    person1->SetAttributeValue("age", "30");

    assert(person1->HasAttribute("firstName"));
    assert(person1->GetAttributeValue("firstName").value() == "Alice");
    assert(person1->GetAttributeValue("age").value() == "30");
    assert(!person1->HasAttribute("nonExistent"));
    assert(!person1->GetAttributeValue("nonExistent").has_value());

    // Querying through session state
    auto retrieved = state.GetInstance("Person", person1->GetInstanceId());
    assert(retrieved != nullptr);
    assert(retrieved->GetInstanceId() == person1->GetInstanceId());
    assert(retrieved->GetAttributeValue("firstName").value() == "Alice");

    auto allPersons = state.GetAllInstances("Person");
    assert(allPersons.size() == 1);
    assert(allPersons[0]->GetInstanceId() == person1->GetInstanceId());

    std::cout << "[PASS] TestInstanceCRUD" << std::endl;
}

void TestRelationships()
{
    SessionEngine::SessionState state;

    auto household = state.CreateInstance("Household");
    auto member1 = state.CreateInstance("Person", household->GetInstanceId(), "members");
    auto member2 = state.CreateInstance("Person", household->GetInstanceId(), "members");

    auto linked = household->GetLinkedInstanceIds("members");
    assert(linked.size() == 2);
    assert(linked[0] == member1->GetInstanceId());
    assert(linked[1] == member2->GetInstanceId());

    household->RemoveRelationshipLink("members", member1->GetInstanceId());
    linked = household->GetLinkedInstanceIds("members");
    assert(linked.size() == 1);
    assert(linked[0] == member2->GetInstanceId());

    std::cout << "[PASS] TestRelationships" << std::endl;
}

void TestCopyAndDeletion()
{
    SessionEngine::SessionState state;

    auto p1 = state.CreateInstance("Person");
    p1->SetAttributeValue("name", "Bob");
    p1->SetAttributeValue("city", "Springfield");

    auto p2 = state.CreateInstance("Person");
    state.CopyInstanceData(p1->GetInstanceId(), p2->GetInstanceId());

    assert(p2->GetAttributeValue("name").value() == "Bob");
    assert(p2->GetAttributeValue("city").value() == "Springfield");

    // Deletion
    bool deleted = state.DeleteInstance("Person", p1->GetInstanceId());
    assert(deleted);
    assert(state.GetInstance("Person", p1->GetInstanceId()) == nullptr);
    assert(state.GetAllInstances("Person").size() == 1);

    std::cout << "[PASS] TestCopyAndDeletion" << std::endl;
}

void TestSerialization()
{
    SessionEngine::SessionState state;

    auto h = state.CreateInstance("Household");
    h->SetAttributeValue("address", "123 Main St");

    auto p = state.CreateInstance("Person", h->GetInstanceId(), "residents");
    p->SetAttributeValue("name", "Charlie");

    std::string jsonStr = state.SerializeToJson();
    assert(!jsonStr.empty());

    SessionEngine::SessionState deserialized;
    bool ok = deserialized.DeserializeFromJson(jsonStr);
    assert(ok);

    auto hList = deserialized.GetAllInstances("Household");
    assert(hList.size() == 1);
    assert(hList[0]->GetInstanceId() == h->GetInstanceId());
    assert(hList[0]->GetAttributeValue("address").value() == "123 Main St");
    assert(hList[0]->GetLinkedInstanceIds("residents").size() == 1);
    assert(hList[0]->GetLinkedInstanceIds("residents")[0] == p->GetInstanceId());

    auto pList = deserialized.GetAllInstances("Person");
    assert(pList.size() == 1);
    assert(pList[0]->GetInstanceId() == p->GetInstanceId());
    assert(pList[0]->GetAttributeValue("name").value() == "Charlie");

    std::cout << "[PASS] TestSerialization" << std::endl;
}

void TestThreadSafety()
{
    SessionEngine::SessionState state;
    std::vector<std::thread> threads;

    for (int t = 0; t < 10; ++t)
    {
        threads.emplace_back([&state, t]() {
            for (int i = 0; i < 50; ++i)
            {
                auto inst = state.CreateInstance("Entity_" + std::to_string(t));
                inst->SetAttributeValue("key", std::to_string(i));
                auto val = inst->GetAttributeValue("key");
                assert(val.has_value() && val.value() == std::to_string(i));
            }
        });
    }

    for (auto& th : threads)
    {
        th.join();
    }

    for (int t = 0; t < 10; ++t)
    {
        auto insts = state.GetAllInstances("Entity_" + std::to_string(t));
        assert(insts.size() == 50);
    }

    std::cout << "[PASS] TestThreadSafety" << std::endl;
}

int main()
{
    std::cout << "Running SessionStateTests..." << std::endl;
    TestInstanceCRUD();
    TestRelationships();
    TestCopyAndDeletion();
    TestSerialization();
    TestThreadSafety();
    std::cout << "All SessionStateTests passed successfully!" << std::endl;
    return 0;
}
