#include "../Thread.h"

#include "Core/ThreadManager.h"
#include "GameAPI/Game.h"
#include "GameAPI/GameCamera.h"
#include "GameAPI/GameEvents.h"
#include "Graph/GraphTable.h"
#include "MCM/MCMTable.h"
#include "Util/ActorUtil.h"

namespace Threading {
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

        // Build actor vector with the new actor to test sorting
        std::vector<GameAPI::GameActor> testActors;
        for (auto& [position, threadActor] : m_actors) {
            testActors.push_back(threadActor.getActor());
        }
        testActors.push_back(GameAPI::GameActor(actor));

        // Sort the test actors (this is what will happen in addActorToThreadInner)
        std::vector<GameAPI::GameActor> dominantActors; // Empty - no explicit dominants
        ActorUtil::sort(testActors, dominantActors, -1); // -1 = auto player position

        // Build conditions in the sorted order
        std::vector<Trait::ActorCondition> conditions;
        for (GameAPI::GameActor& sortedActor : testActors) {
            conditions.push_back(Trait::ActorCondition::create(sortedActor.form));
        }

        // Check if a compatible node exists for the sorted actor configuration
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

    bool Thread::addActorToThread(RE::Actor* actor, bool useFades) {
        // Validate we can add this actor
        if (!canAddActor(actor)) {
            logger::warn("Cannot add actor: validation failed");
            return false;
        }

        GameAPI::GameActor gameActor(actor);
        logger::info("Adding actor {} to thread {} (useFades={})", gameActor.getName(), m_threadId, useFades);

        // Build new actor list including the new actor
        std::vector<GameAPI::GameActor> newActors;
        for (auto& [index, threadActor] : m_actors) {
            newActors.push_back(threadActor.getActor());
        }
        newActors.push_back(gameActor);

        // Use migration for clean thread recreation with proper events
        int newThreadId = ThreadManager::GetSingleton()->migrateThread(m_threadId, newActors);
        
        if (newThreadId >= 0) {
            std::string message = gameActor.isPlayer() ? "Player joined scene" : "Actor added to scene";
            logger::info("{} - new thread ID: {}", message, newThreadId);
            GameAPI::Game::notification(message);
            return true;
        } else {
            logger::error("Failed to migrate thread when adding actor");
            GameAPI::Game::notification("Failed to add actor");
            return false;
        }
    }

    void Thread::addActorWithUI() {
        logger::info("addActorWithUI called, current actor count: {}", m_actors.size());

        // Get nearby actors from player's perspective
        std::vector<GameAPI::GameActor> nearbyActors = GameAPI::GameActor::getPlayer().getNearbyActors(2000.0f, [this](GameAPI::GameActor actor) {
            return canAddActor(actor.form);
        });

        // Convert to RE::Actor* vector
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

        // Build message box options
        std::vector<std::string> actorNames = {"$ostim_message_none"};
        for (RE::Actor* actor : validActors) {
            actorNames.push_back(GameAPI::GameActor(actor).getName());
        }

        // Capture thread ID for async callback
        ThreadId threadId = m_threadId;

        // Show message box to select actor
        GameAPI::Game::showMessageBox("$ostim_message_add_actor", actorNames,
            [threadId, validActors](unsigned int result) {
                if (result == 0 || result > validActors.size()) {
                    // User cancelled or invalid selection
                    return;
                }

                // Get thread again (important for async safety)
                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (!thread) {
                    return;
                }

                // Add the selected actor
                RE::Actor* selectedActor = validActors[result - 1];
                bool success = thread->addActorToThread(selectedActor);

                if (success) {
                    GameAPI::Game::notification("Actor added successfully");
                } else {
                    GameAPI::Game::notification("Failed to add actor");
                }
            });
    }

    bool Thread::canRemoveActor(int position) {
        // Check if position exists
        if (m_actors.find(position) == m_actors.end()) {
            logger::info("Cannot remove actor: position {} doesn't exist", position);
            return false;
        }

        // Ensure at least 1 actor remains (minimum for a thread)
        if (m_actors.size() <= 1) {
            logger::info("Cannot remove actor: would leave thread with no actors");
            return false;
        }

        // Build actor vector without the removed actor to test sorting
        std::vector<GameAPI::GameActor> testActors;
        for (auto& [idx, threadActor] : m_actors) {
            if (idx != position) {
                testActors.push_back(threadActor.getActor());
            }
        }

        // Sort the remaining actors (this is what will happen in removeActorFromThreadInner)
        std::vector<GameAPI::GameActor> dominantActors; // Empty - no explicit dominants
        ActorUtil::sort(testActors, dominantActors, -1); // -1 = auto player position

        // Build conditions in the sorted order
        std::vector<Trait::ActorCondition> conditions;
        for (GameAPI::GameActor& sortedActor : testActors) {
            conditions.push_back(Trait::ActorCondition::create(sortedActor.form));
        }

        // Check if a compatible node exists for the sorted remaining actors
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

    bool Thread::removeActorFromThread(int position, bool useFades) {
        if (!canRemoveActor(position)) {
            return false;
        }

        ThreadActor* actorToRemove = GetActor(position);
        if (!actorToRemove) {
            logger::error("Cannot remove actor: null pointer at position {}", position);
            return false;
        }

        GameAPI::GameActor removingActor = actorToRemove->getActor();
        logger::info("Removing actor {} from thread {} (useFades={})", removingActor.getName(), m_threadId, useFades);

        // Build new actor list without the removed actor
        std::vector<GameAPI::GameActor> newActors;
        for (auto& [idx, threadActor] : m_actors) {
            if (idx != position) {
                newActors.push_back(threadActor.getActor());
            }
        }

        // Check if any actors remain
        if (newActors.empty()) {
            logger::info("No actors remaining - stopping thread");
            stop();
            return true;
        }

        // Use migration for clean thread recreation with proper events
        int newThreadId = ThreadManager::GetSingleton()->migrateThread(m_threadId, newActors);
        
        if (newThreadId >= 0) {
            std::string message = removingActor.isPlayer() ? "Player left scene" : "Actor removed from scene";
            logger::info("{} - new thread ID: {}", message, newThreadId);
            GameAPI::Game::notification(message);
            return true;
        } else {
            logger::error("Failed to migrate thread when removing actor - stopping thread");
            GameAPI::Game::notification("Failed to remove actor");
            stop();
            return false;
        }
    }

    void Thread::removeActorWithUI() {
        logger::info("removeActorWithUI called, actor count: {}", m_actors.size());

        // Check if thread has at least 2 actors (need to keep at least 1)
        if (m_actors.size() <= 1) {
            logger::warn("Cannot remove actor: only {} actor(s) in thread", m_actors.size());
            GameAPI::Game::notification("Cannot remove actor: minimum 1 actor required");
            return;
        }

        // Build list of removable actors
        std::vector<std::string> actorNames = {"$ostim_message_none"};
        std::vector<int> actorPositions;

        int maxOptions = GameAPI::Game::getMessageBoxOptionLimit();
        int count = 0;
        for (auto& [position, actor] : m_actors) {
            if (count >= maxOptions) {
                break;
            }
            if (canRemoveActor(position)) {
                // Include position in name for clarity
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

        // Capture thread ID for async callback
        ThreadId threadId = m_threadId;

        // Show message box to select actor to remove
        GameAPI::Game::showMessageBox("$ostim_message_remove_actor", actorNames,
            [threadId, actorPositions](unsigned int result) {
                if (result == 0 || result > actorPositions.size()) {
                    // User cancelled or invalid selection
                    return;
                }

                // Get thread again (important for async safety)
                Thread* thread = ThreadManager::GetSingleton()->GetThread(threadId);
                if (!thread) {
                    return;
                }

                // Remove the selected actor
                int selectedPosition = actorPositions[result - 1];
                bool success = thread->removeActorFromThread(selectedPosition);

                if (success) {
                    GameAPI::Game::notification("Actor removed successfully");
                } else {
                    GameAPI::Game::notification("Failed to remove actor");
                }
            });
    }
}
