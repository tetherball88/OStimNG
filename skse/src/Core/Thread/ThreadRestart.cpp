#include "../Thread.h"

#include "Core/ThreadManager.h"
#include "Furniture/FurnitureType.h"
#include "GameAPI/Game.h"
#include "GameAPI/GameCamera.h"
#include "Graph/GraphTable.h"
#include "MCM/MCMTable.h"
#include "Trait/Condition.h"
#include "Util/ActorUtil.h"

namespace Threading {
    // State to preserve for each actor during thread restart
    struct ActorTransferState {
        RE::Actor* actor = nullptr;
        float excitement = 0;
        int timesClimaxed = 0;
    };

    // State to transfer between old and new thread
    struct ThreadTransferState {
        std::vector<ActorTransferState> actorStates;
        GameAPI::GameObject furniture;
        float relativeSpeed = -1;
        ThreadFlags threadFlags = 0;
        std::vector<std::string> metadata;
        Graph::Node* currentNode = nullptr;
    };

    // Capture state from current thread before stopping
    ThreadTransferState captureThreadState(Thread* thread) {
        ThreadTransferState state;

        // Capture furniture
        state.furniture = thread->getFurniture();

        // Capture thread flags
        state.threadFlags = thread->getThreadFlags();

        // Capture relative speed for restoration
        state.relativeSpeed = thread->getRelativeSpeed();

        // Capture metadata
        state.metadata = thread->metadata.getMetadata();

        // Capture current node for potential reuse (e.g., swap actors)
        state.currentNode = thread->getCurrentNodeInternal();

        // Capture actor states
        for (auto& [position, threadActor] : thread->getActors()) {
            ActorTransferState actorState;
            actorState.actor = threadActor.getActor().form;
            actorState.excitement = threadActor.getExcitement();
            actorState.timesClimaxed = threadActor.getTimexClimaxed();
            state.actorStates.push_back(actorState);
        }

        return state;
    }

    // Restore excitement levels to actors in new thread
    void restoreActorStates(Thread* newThread, const std::vector<ActorTransferState>& oldStates) {
        for (const ActorTransferState& oldState : oldStates) {
            ThreadActor* newActor = newThread->GetActor(GameAPI::GameActor(oldState.actor));
            if (newActor) {
                newActor->setExcitement(oldState.excitement);
                // Note: timesClimaxed is private, would need a setter if we want to restore it
            }
        }
    }

    // Find a compatible node for the given actors
    Graph::Node* findCompatibleNode(Furniture::FurnitureType* furnitureType,
                                     const std::vector<GameAPI::GameActor>& actors) {
        std::vector<Trait::ActorCondition> conditions;
        for (const GameAPI::GameActor& actor : actors) {
            conditions.push_back(Trait::ActorCondition::create(actor.form));
        }

        return Graph::GraphTable::getRandomNode(
            furnitureType,
            conditions,
            [](Graph::Node* node) { return !node->isTransition; });
    }

    // Check if a node is compatible with the given actors (used for preserving scene during swap)
    bool isNodeCompatibleWithActors(Graph::Node* node, const std::vector<GameAPI::GameActor>& actors) {
        if (!node || node->actors.size() != actors.size()) {
            return false;
        }

        for (size_t i = 0; i < actors.size(); i++) {
            Trait::ActorCondition condition = Trait::ActorCondition::create(actors[i].form);
            if (!condition.fulfills(node->actors[i].condition)) {
                return false;
            }
        }

        return true;
    }

    // Core restart implementation - stops old thread, starts new one
    // Returns new thread ID or -1 on failure
    // If preferredNode is provided and compatible, it will be used instead of finding a random node
    int restartThreadWithActors(ThreadId oldThreadId, std::vector<GameAPI::GameActor> newActors, Graph::Node* preferredNode = nullptr) {
        ThreadManager* manager = ThreadManager::GetSingleton();
        Thread* oldThread = manager->GetThread(oldThreadId);

        if (!oldThread) {
            logger::warn("restartThreadWithActors: old thread {} not found", oldThreadId);
            return -1;
        }

        // Sort new actors in proper order
        std::vector<GameAPI::GameActor> dominantActors;
        ActorUtil::sort(newActors, dominantActors, -1);

        // Capture state before stopping
        ThreadTransferState transferState = captureThreadState(oldThread);

        // Get furniture type for node lookup
        Furniture::FurnitureType* furnitureType = oldThread->getFurnitureType();

        // Determine starting node - prefer the provided node if compatible
        Graph::Node* startingNode = nullptr;
        if (preferredNode && isNodeCompatibleWithActors(preferredNode, newActors)) {
            startingNode = preferredNode;
            logger::info("restartThreadWithActors: using preferred node {}", preferredNode->scene_id);
        } else {
            // Find compatible node for new actor configuration
            startingNode = findCompatibleNode(furnitureType, newActors);
        }

        if (!startingNode) {
            logger::warn("restartThreadWithActors: no compatible node found for {} actors", newActors.size());
            return -1;
        }

        // Build start params for new thread
        ThreadStartParams params;
        params.actors = newActors;
        params.furniture = transferState.furniture;
        params.threadFlags = transferState.threadFlags;
        params.metadata = transferState.metadata;
        params.startingNodes = {{startingNode->animationLengthMs, startingNode}};

        // Atomically restart the thread - stops old, starts new
        // This fires end event for old thread and start event for new thread
        // New thread ID is assigned correctly (0 for player, new ID for NPC)
        int newThreadId = manager->restartThread(oldThreadId, params);

        if (newThreadId >= 0) {
            Thread* newThread = manager->GetThread(newThreadId);
            if (newThread) {
                // Restore excitement levels to actors that were in old thread
                restoreActorStates(newThread, transferState.actorStates);

                // Try to restore speed if the new node supports it
                if (transferState.relativeSpeed >= 0 && newThread->getCurrentNodeInternal()) {
                    int targetSpeed = static_cast<int>(
                        transferState.relativeSpeed *
                        (newThread->getCurrentNodeInternal()->speeds.size() - 1) + 0.5);
                    newThread->SetSpeed(targetSpeed);
                }
            }
        }

        return newThreadId;
    }

    // =========================================================================
    // Add Actor Implementation
    // =========================================================================

    bool Thread::canAddActor(RE::Actor* actor) {
        if (!actor) {
            logger::info("Cannot add actor: null actor pointer");
            return false;
        }

        std::string actorName = GameAPI::GameActor(actor).getName();

        // Check if actor is already in thread
        if (GetActor(GameAPI::GameActor(actor)) != nullptr) {
            logger::info("Cannot add actor {}: actor already in thread", actorName);
            return false;
        }

        // Check if actor is locked by another thread
        if (ThreadManager::GetSingleton()->findThread(GameAPI::GameActor(actor))) {
            logger::info("Cannot add actor {}: actor is in another thread", actorName);
            return false;
        }

        // Build actor vector with the new actor to test compatibility
        std::vector<GameAPI::GameActor> testActors;
        for (auto& [position, threadActor] : m_actors) {
            testActors.push_back(threadActor.getActor());
        }
        testActors.push_back(GameAPI::GameActor(actor));

        // Sort the test actors
        std::vector<GameAPI::GameActor> dominantActors;
        ActorUtil::sort(testActors, dominantActors, -1);

        // Build conditions in the sorted order
        std::vector<Trait::ActorCondition> conditions;
        for (GameAPI::GameActor& sortedActor : testActors) {
            conditions.push_back(Trait::ActorCondition::create(sortedActor.form));
        }

        // Check if a compatible node exists
        Graph::Node* testNode = Graph::GraphTable::getRandomNode(
            furnitureType,
            conditions,
            [](Graph::Node* node) { return !node->isTransition; });

        if (!testNode) {
            logger::info("Cannot add actor {}: no compatible node found for {} actors", actorName, conditions.size());
            return false;
        }

        return true;
    }

    void Thread::addActorToThreadInner(RE::Actor* actor) {
        if (!canAddActor(actor)) {
            logger::warn("Cannot add actor: validation failed");
            return;
        }

        logger::info("Adding actor to thread via restart: {}", GameAPI::GameActor(actor).getName());

        // Build new actor list including the new actor
        std::vector<GameAPI::GameActor> newActors;
        for (auto& [position, threadActor] : m_actors) {
            newActors.push_back(threadActor.getActor());
        }
        newActors.push_back(GameAPI::GameActor(actor));

        // Restart thread with new actors
        int newThreadId = restartThreadWithActors(m_threadId, newActors);

        if (newThreadId >= 0) {
            logger::info("Successfully added actor via thread restart, new thread ID: {}", newThreadId);
        } else {
            logger::error("Failed to restart thread when adding actor");
        }
    }

    bool Thread::addActorToThread(RE::Actor* actor, bool useFades) {
        if (!canAddActor(actor)) {
            return false;
        }

        if (playerThread && useFades && MCM::MCMTable::useFades()) {
            // Capture actor and thread ID before detached thread
            RE::Actor* actorCopy = actor;
            ThreadId threadId = m_threadId;

            std::thread fadeThread = std::thread([actorCopy, threadId] {
                GameAPI::GameCamera::fadeToBlack(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(700));

                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (thread) {
                    thread->addActorToThreadInner(actorCopy);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(550));
                GameAPI::GameCamera::fadeFromBlack(1);
            });
            fadeThread.detach();
        } else {
            addActorToThreadInner(actor);
        }

        return true;
    }

    void Thread::addActorWithUI() {
        logger::info("addActorWithUI called, current actor count: {}", m_actors.size());

        std::vector<GameAPI::GameActor> nearbyActors = GameAPI::GameActor::getPlayer().getNearbyActors(2000.0f, [this](GameAPI::GameActor actor) {
            return canAddActor(actor.form);
        });

        std::vector<RE::Actor*> validActors;
        for (GameAPI::GameActor& gameActor : nearbyActors) {
            if (gameActor.form) {
                validActors.push_back(gameActor.form);
            }
        }

        if (validActors.empty()) {
            GameAPI::Game::notification("No valid actors nearby to add");
            logger::info("No valid actors found nearby");
            return;
        }

        std::vector<std::string> actorNames = {"$ostim_message_none"};
        for (RE::Actor* actor : validActors) {
            actorNames.push_back(GameAPI::GameActor(actor).getName());
        }

        ThreadId threadId = m_threadId;

        GameAPI::Game::showMessageBox("$ostim_message_add_actor", actorNames,
            [threadId, validActors](unsigned int result) {
                if (result == 0 || result > validActors.size()) {
                    return;
                }

                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (!thread) {
                    return;
                }

                RE::Actor* selectedActor = validActors[result - 1];
                bool success = thread->addActorToThread(selectedActor);

                if (success) {
                    GameAPI::Game::notification("Actor added successfully");
                } else {
                    GameAPI::Game::notification("Failed to add actor");
                }
            });
    }

    // =========================================================================
    // Remove Actor Implementation
    // =========================================================================

    bool Thread::canRemoveActor(int position) {
        if (m_actors.find(position) == m_actors.end()) {
            logger::info("Cannot remove actor: position {} doesn't exist", position);
            return false;
        }

        // Ensure at least 1 actor remains
        if (m_actors.size() <= 1) {
            logger::info("Cannot remove actor: would leave thread with no actors");
            return false;
        }

        // Build actor vector without the removed actor
        std::vector<GameAPI::GameActor> testActors;
        for (auto& [idx, threadActor] : m_actors) {
            if (idx != position) {
                testActors.push_back(threadActor.getActor());
            }
        }

        // Sort the remaining actors
        std::vector<GameAPI::GameActor> dominantActors;
        ActorUtil::sort(testActors, dominantActors, -1);

        // Build conditions
        std::vector<Trait::ActorCondition> conditions;
        for (GameAPI::GameActor& sortedActor : testActors) {
            conditions.push_back(Trait::ActorCondition::create(sortedActor.form));
        }

        // Check if a compatible node exists
        Graph::Node* testNode = Graph::GraphTable::getRandomNode(
            furnitureType,
            conditions,
            [](Graph::Node* node) { return !node->isTransition; });

        if (!testNode) {
            logger::info("Cannot remove actor: no compatible node found for {} actors", conditions.size());
            return false;
        }

        return true;
    }

    void Thread::removeActorFromThreadInner(int position) {
        logger::info("Removing actor from thread at position: {} via restart", position);

        // Build new actor list excluding the removed actor
        std::vector<GameAPI::GameActor> newActors;
        for (auto& [idx, threadActor] : m_actors) {
            if (idx != position) {
                newActors.push_back(threadActor.getActor());
            }
        }

        if (newActors.empty()) {
            logger::error("Cannot remove actor: would leave thread with no actors");
            return;
        }

        // Restart thread with remaining actors
        int newThreadId = restartThreadWithActors(m_threadId, newActors);

        if (newThreadId >= 0) {
            logger::info("Successfully removed actor via thread restart, new thread ID: {}", newThreadId);
        } else {
            logger::error("Failed to restart thread when removing actor");
        }
    }

    bool Thread::removeActorFromThread(int position, bool useFades) {
        if (!canRemoveActor(position)) {
            return false;
        }

        if (playerThread && useFades && MCM::MCMTable::useFades()) {
            ThreadId threadId = m_threadId;
            int positionCopy = position;

            std::thread fadeThread = std::thread([threadId, positionCopy] {
                GameAPI::GameCamera::fadeToBlack(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(700));

                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (thread) {
                    thread->removeActorFromThreadInner(positionCopy);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(550));
                GameAPI::GameCamera::fadeFromBlack(1);
            });
            fadeThread.detach();
        } else {
            removeActorFromThreadInner(position);
        }

        return true;
    }

    void Thread::removeActorWithUI() {
        logger::info("removeActorWithUI called, actor count: {}", m_actors.size());

        if (m_actors.size() <= 1) {
            logger::warn("Cannot remove actor: only {} actor(s) in thread", m_actors.size());
            GameAPI::Game::notification("Cannot remove actor: minimum 1 actor required");
            return;
        }

        std::vector<std::string> actorNames = {"$ostim_message_none"};
        std::vector<int> actorPositions;

        int maxOptions = GameAPI::Game::getMessageBoxOptionLimit();
        int count = 0;
        for (auto& [position, actor] : m_actors) {
            if (count >= maxOptions) {
                break;
            }
            if (canRemoveActor(position)) {
                std::string displayName = actor.getActor().getName() + " (Position " + std::to_string(position) + ")";
                actorNames.push_back(displayName);
                actorPositions.push_back(position);
                count++;
            }
        }

        if (actorPositions.empty()) {
            GameAPI::Game::notification("Cannot remove any actors from this scene");
            logger::info("No removable actors found");
            return;
        }

        ThreadId threadId = m_threadId;

        GameAPI::Game::showMessageBox("$ostim_message_remove_actor", actorNames,
            [threadId, actorPositions](unsigned int result) {
                if (result == 0 || result > actorPositions.size()) {
                    return;
                }

                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (!thread) {
                    return;
                }

                int selectedPosition = actorPositions[result - 1];
                bool success = thread->removeActorFromThread(selectedPosition);

                if (success) {
                    GameAPI::Game::notification("Actor removed successfully");
                } else {
                    GameAPI::Game::notification("Failed to remove actor");
                }
            });
    }

    // =========================================================================
    // Swap Actors Implementation
    // =========================================================================

    bool Thread::canSwapActors(int positionA, int positionB) {
        if (m_actors.find(positionA) == m_actors.end() || m_actors.find(positionB) == m_actors.end()) {
            logger::info("Cannot swap actors: one or both positions don't exist (positions {} and {})", positionA, positionB);
            return false;
        }

        if (positionA == positionB) {
            logger::info("Cannot swap actors: same position");
            return false;
        }

        ThreadActor* actorA = GetActor(positionA);
        ThreadActor* actorB = GetActor(positionB);

        if (!actorA || !actorB) {
            logger::info("Cannot swap actors: null actor pointer");
            return false;
        }

        if (MCM::MCMTable::unrestrictedNavigation()) {
            return true;
        }

        Trait::ActorCondition conditionA = Trait::ActorCondition::create(actorA);
        Trait::ActorCondition conditionB = Trait::ActorCondition::create(actorB);

        if (MCM::MCMTable::intendedSexOnly()) {
            if (conditionA.sex != conditionB.sex &&
                conditionA.sex != GameAPI::GameSex::AGENDER &&
                conditionB.sex != GameAPI::GameSex::AGENDER) {
                logger::info("Cannot swap actors: different sexes - {} (sex: {}) and {} (sex: {})",
                    actorA->getActor().getName(), static_cast<int>(conditionA.sex),
                    actorB->getActor().getName(), static_cast<int>(conditionB.sex));
                return false;
            }
        }

        // Build swapped actor list and check for compatible node
        std::vector<GameAPI::GameActor> swappedActors;
        for (auto& [position, threadActor] : m_actors) {
            swappedActors.push_back(threadActor.getActor());
        }
        std::swap(swappedActors[positionA], swappedActors[positionB]);

        std::vector<Trait::ActorCondition> conditions;
        for (GameAPI::GameActor& actor : swappedActors) {
            conditions.push_back(Trait::ActorCondition::create(actor.form));
        }

        Graph::Node* testNode = Graph::GraphTable::getRandomNode(
            furnitureType,
            conditions,
            [](Graph::Node* node) { return !node->isTransition; });

        if (!testNode) {
            logger::info("Cannot swap actors: no valid node found for swapped configuration");
            return false;
        }

        return true;
    }

    bool Thread::isNodeValidForSwap(Graph::Node* node, int positionA, int positionB) {
        if (!node || positionA >= node->actors.size() || positionB >= node->actors.size()) {
            return false;
        }

        ThreadActor* actorA = GetActor(positionA);
        ThreadActor* actorB = GetActor(positionB);

        if (!actorA || !actorB) {
            return false;
        }

        if (MCM::MCMTable::unrestrictedNavigation()) {
            return true;
        }

        Trait::ActorCondition conditionA = Trait::ActorCondition::create(actorA);
        Trait::ActorCondition conditionB = Trait::ActorCondition::create(actorB);

        if (!conditionA.fulfills(node->actors[positionB].condition)) {
            return false;
        }

        if (!conditionB.fulfills(node->actors[positionA].condition)) {
            return false;
        }

        return true;
    }

    std::vector<int> Thread::getSwapPartners(GameAPI::GameActor actor) {
        std::vector<int> swapPartners;

        int actorPosition = getActorPosition(actor);
        if (actorPosition == -1) {
            return swapPartners;
        }

        for (auto& [index, threadActor] : m_actors) {
            if (index != actorPosition && canSwapActors(actorPosition, index)) {
                swapPartners.push_back(index);
            }
        }

        return swapPartners;
    }

    void Thread::swapActorsInner(int positionA, int positionB) {
        logger::info("Swapping actors at positions {} and {} via restart", positionA, positionB);

        // Build actor list with swapped positions
        std::vector<GameAPI::GameActor> swappedActors;
        for (auto& [position, threadActor] : m_actors) {
            swappedActors.push_back(threadActor.getActor());
        }
        std::swap(swappedActors[positionA], swappedActors[positionB]);

        // Get current node to preserve the scene during swap
        Graph::Node* currentNode = getCurrentNodeInternal();

        // Restart thread with swapped actors, preserving the current scene
        int newThreadId = restartThreadWithActors(m_threadId, swappedActors, currentNode);

        if (newThreadId >= 0) {
            logger::info("Successfully swapped actors via thread restart, new thread ID: {}", newThreadId);
        } else {
            logger::error("Failed to restart thread when swapping actors");
        }
    }

    bool Thread::swapActors(int positionA, int positionB, bool useFades) {
        if (!canSwapActors(positionA, positionB)) {
            return false;
        }

        if (playerThread && useFades && MCM::MCMTable::useFades()) {
            ThreadId threadId = m_threadId;

            std::thread fadeThread = std::thread([threadId, positionA, positionB] {
                GameAPI::GameCamera::fadeToBlack(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(700));

                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (thread) {
                    thread->swapActorsInner(positionA, positionB);
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(550));
                GameAPI::GameCamera::fadeFromBlack(1);
            });
            fadeThread.detach();
        } else {
            swapActorsInner(positionA, positionB);
        }

        return true;
    }

    void Thread::swapActorsWithUI() {
        logger::info("swapActorsWithUI called, actor count: {}", m_actors.size());

        if (m_actors.size() < 2) {
            logger::warn("Not enough actors to swap: {}", m_actors.size());
            GameAPI::Game::notification("Not enough actors to swap");
            return;
        }

        std::vector<std::string> actorNames = {"$ostim_message_none"};
        std::vector<int> actorPositions;

        int maxOptions = GameAPI::Game::getMessageBoxOptionLimit();
        int count = 0;
        for (auto& [position, actor] : m_actors) {
            if (count >= maxOptions) {
                break;
            }
            actorNames.push_back(actor.getActor().getName());
            actorPositions.push_back(position);
            count++;
        }

        ThreadId threadId = m_threadId;

        GameAPI::Game::showMessageBox("$ostim_message_swap_actors_first", actorNames,
            [threadId, actorPositions](unsigned int result) {
                if (result == 0 || result > actorPositions.size()) {
                    return;
                }

                int positionA = actorPositions[result - 1];

                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (!thread) {
                    return;
                }

                std::vector<std::string> partnerNames = {"$ostim_message_none"};
                std::vector<int> partnerPositions;

                int maxOptions = GameAPI::Game::getMessageBoxOptionLimit();
                int count = 0;
                for (auto& [position, actor] : thread->m_actors) {
                    if (count >= maxOptions) {
                        break;
                    }
                    if (position != positionA && thread->canSwapActors(positionA, position)) {
                        partnerNames.push_back(actor.getActor().getName());
                        partnerPositions.push_back(position);
                        count++;
                    }
                }

                if (partnerNames.size() <= 1) {
                    GameAPI::Game::notification("No valid actors to swap with selected actor");
                    return;
                }

                GameAPI::Game::showMessageBox("$ostim_message_swap_actors_second", partnerNames,
                    [threadId, positionA, partnerPositions](unsigned int result) {
                        if (result == 0 || result > partnerPositions.size()) {
                            return;
                        }

                        int positionB = partnerPositions[result - 1];

                        Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                        if (!thread) {
                            return;
                        }

                        bool success = thread->swapActors(positionA, positionB);
                        if (success) {
                            GameAPI::Game::notification("Actors swapped successfully");
                        } else {
                            GameAPI::Game::notification("Failed to swap actors");
                        }
                    });
            });
    }
}
