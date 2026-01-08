#include "../Thread.h"

#include "Core/ThreadManager.h"
#include "GameAPI/Game.h"
#include "GameAPI/GameCamera.h"
#include "GameAPI/GameEvents.h"
#include "Graph/GraphTable.h"
#include "MCM/MCMTable.h"

namespace Threading {
    bool Thread::canSwapActors(int positionA, int positionB) {
        // Check if both positions exist
        if (m_actors.find(positionA) == m_actors.end() || m_actors.find(positionB) == m_actors.end()) {
            logger::info("Cannot swap actors: one or both positions don't exist (positions {} and {})", positionA, positionB);
            return false;
        }

        // Don't allow swapping with self
        if (positionA == positionB) {
            logger::info("Cannot swap actors: same position");
            return false;
        }

        // Get the actors
        ThreadActor* actorA = GetActor(positionA);
        ThreadActor* actorB = GetActor(positionB);

        if (!actorA || !actorB) {
            logger::info("Cannot swap actors: null actor pointer");
            return false;
        }

        // Skip condition checks if unrestrictedNavigation is enabled
        if (MCM::MCMTable::unrestrictedNavigation()) {
            return true;
        }

        // Get actor conditions
        Trait::ActorCondition conditionA = Trait::ActorCondition::create(actorA);
        Trait::ActorCondition conditionB = Trait::ActorCondition::create(actorB);

        // Check if actors have the same sex (or one is AGENDER which can match both)
        // Only enforce this check if intendedSexOnly is enabled
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

        // Check if current node would still be valid with swapped positions
        if (m_currentNode && !isNodeValidForSwap(m_currentNode, positionA, positionB)) {
            // Try to find an alternative node
            std::vector<Trait::ActorCondition> conditions = getActorConditions();
            std::swap(conditions[positionA], conditions[positionB]);

            Graph::Node* alternativeNode = Graph::GraphTable::getRandomNode(furnitureType, conditions,
                [](Graph::Node* node) { return !node->isTransition; });

            if (!alternativeNode) {
                logger::info("Cannot swap actors: no valid alternative node found");
                return false;
            }
        }

        return true;
    }

    bool Thread::isNodeValidForSwap(Graph::Node* node, int positionA, int positionB) {
        if (!node || positionA >= node->actors.size() || positionB >= node->actors.size()) {
            return false;
        }

        // Get actor conditions
        ThreadActor* actorA = GetActor(positionA);
        ThreadActor* actorB = GetActor(positionB);

        if (!actorA || !actorB) {
            return false;
        }

        // Skip condition checks if unrestrictedNavigation is enabled
        if (MCM::MCMTable::unrestrictedNavigation()) {
            return true;
        }

        Trait::ActorCondition conditionA = Trait::ActorCondition::create(actorA);
        Trait::ActorCondition conditionB = Trait::ActorCondition::create(actorB);

        // Check if actor A can fulfill position B's requirements
        if (!conditionA.fulfills(node->actors[positionB].condition)) {
            return false;
        }

        // Check if actor B can fulfill position A's requirements
        if (!conditionB.fulfills(node->actors[positionA].condition)) {
            return false;
        }

        return true;
    }

    std::vector<int> Thread::getSwapPartners(GameAPI::GameActor actor) {
        std::vector<int> swapPartners;

        // Find the position of the given actor in the thread
        int actorPosition = getActorPosition(actor);
        if (actorPosition == -1) {
            // Actor not found in thread
            return swapPartners;
        }

        // Check all other actors to see if they can swap with the given actor
        for (auto& [index, threadActor] : m_actors) {
            if (index != actorPosition && canSwapActors(actorPosition, index)) {
                swapPartners.push_back(index);
            }
        }

        return swapPartners;
    }

    bool Thread::swapActors(int positionA, int positionB, bool useFades) {
        if (!canSwapActors(positionA, positionB)) {
            return false;
        }

        logger::info("Swapping actors at positions {} and {} in thread {} (useFades={})", positionA, positionB, m_threadId, useFades);

        // Build actor list with swapped positions
        std::vector<GameAPI::GameActor> newActors;
        for (auto& [index, threadActor] : m_actors) {
            newActors.push_back(threadActor.getActor());
        }
        // Swap the actors in the vector
        std::swap(newActors[positionA], newActors[positionB]);

        // Use migration for clean thread recreation with proper events
        int newThreadId = ThreadManager::GetSingleton()->migrateThread(m_threadId, newActors);
        
        if (newThreadId >= 0) {
            logger::info("Successfully swapped actors - new thread ID: {}", newThreadId);
            GameAPI::Game::notification("Actors swapped");
            return true;
        } else {
            logger::error("Failed to migrate thread when swapping actors");
            GameAPI::Game::notification("Failed to swap actors");
            return false;
        }
    }

    void Thread::swapActorsWithUI() {
        logger::info("swapActorsWithUI called, actor count: {}", m_actors.size());

        // Check if thread has at least 2 actors
        if (m_actors.size() < 2) {
            logger::warn("Not enough actors to swap: {}", m_actors.size());
            GameAPI::Game::notification("Not enough actors to swap");
            return;
        }

        // Build list of actors for first selection
        // Following OStim pattern: first option is message, actual items start at index 1
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

        // Capture thread ID for lambda callbacks
        ThreadId threadId = m_threadId;

        // Show first message box to select Actor A
        GameAPI::Game::showMessageBox("$ostim_message_swap_actors_first", actorNames,
            [threadId, actorPositions](unsigned int result) {
                if (result == 0 || result > actorPositions.size()) {
                    // User cancelled or invalid selection
                    return;
                }

                // Get selected actor A position (result is 1-indexed)
                int positionA = actorPositions[result - 1];

                // Get thread again (important for async safety)
                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (!thread) {
                    return;
                }

                // Build list of valid swap partners for actor A
                // Following OStim pattern: first option is "Select second actor", actual items start at index 1
                std::vector<std::string> partnerNames = {"$ostim_message_none"};
                std::vector<int> partnerPositions;

                int maxOptions = GameAPI::Game::getMessageBoxOptionLimit();
                int count = 0;
                for (auto& [position, actor] : thread->m_actors) {
                    if (count >= maxOptions) {
                        break;
                    }
                    // Skip actor A itself and check if swap is valid
                    if (position != positionA && thread->canSwapActors(positionA, position)) {
                        partnerNames.push_back(actor.getActor().getName());
                        partnerPositions.push_back(position);
                        count++;
                    }
                }

                // Check if there are any valid swap partners
                if (partnerNames.empty()) {
                    GameAPI::Game::notification("No valid actors to swap with selected actor");
                    return;
                }

                // Show second message box to select Actor B
                GameAPI::Game::showMessageBox("$ostim_message_swap_actors_second", partnerNames,
                    [threadId, positionA, partnerPositions](unsigned int result) {
                        // Following OStim pattern: result == 0 means cancel
                        if (result == 0 || result > partnerPositions.size()) {
                            // User cancelled or invalid selection
                            return;
                        }

                        // Get selected actor B position
                        int positionB = partnerPositions[result - 1];

                        // Get thread again for final swap
                        Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                        if (!thread) {
                            return;
                        }

                        // Perform the swap
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
