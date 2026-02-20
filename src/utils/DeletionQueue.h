#pragma once
#include <deque>
#include <functional>

struct DeletionQueue {
    // A queue holding functions that take no arguments and return void
    std::deque<std::function<void()>> deletors;

    // Add a new lambda to the queue
    void push_function(std::function<void()>&& function) {
        deletors.push_back(function);
    }

    // Execute all lambdas and clear the queue
    void flush() {
        // We iterate in reverse. If we pushed a Parent object and then a Child object, 
        // we want to destroy the Child FIRST before the Parent it relies on.
        for (auto it = deletors.rbegin(); it != deletors.rend(); ++it) {
            (*it)(); // Call the lambda
        }

        deletors.clear();
    }
};