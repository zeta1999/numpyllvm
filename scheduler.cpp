
#include "initializers.hpp"
#include "scheduler.hpp"
#include "thread.hpp"
#include "Include/blockingconcurrentqueue.h"
#include "compiler.hpp"

moodycamel::BlockingConcurrentQueue<Task*> queue;
bool running = true;

static void ScheduleFunction(JITFunction *jf);

void
ScheduleCompilation(Pipeline *pipeline) {
    PipelineNode *node = pipeline->children;
    while(node) {
        ScheduleCompilation(node->child);
        node = node->next;
    }
    CompileTask* task = (CompileTask*) calloc(1, sizeof(CompileTask));
    task->type = tasktype_compile;
    task->pipeline = pipeline;
    ScheduleTask((Task*) task);
}

void
ScheduleExecution(Pipeline *pipeline) {
    PipelineNode *node = pipeline->children;
    bool all_evaluated = true;
    // schedule a pipeline for execution
    semaphore_wait(&pipeline->lock);
    // check if the pipeline has already been scheduled
    if (pipeline->scheduled_for_execution) {
        printf("Failed to schedule pipeline %s - Already Scheduled\n", pipeline->name);
        goto unlock;
    }
    // if not, check if all children have been evaluated
    while(node) {
        if (!node->child->evaluated) {
            all_evaluated = false;
            break;
        }
        node = node->next;
    }
    if (!all_evaluated) {
        printf("Failed to schedule pipeline %s - Children Not Evaluated\n", pipeline->name);
        goto unlock;
    }

    // if all children have to be evaluated, check if the function has been compiled
    if (!pipeline->function) {
        printf("Failed to schedule pipeline %s - Not Compiled\n", pipeline->name);
        goto unlock;
    }
    pipeline->scheduled_for_execution = true;
    ScheduleFunction(pipeline->function);
unlock:
    semaphore_increment(&pipeline->lock);
}

static void
ScheduleFunction(JITFunction *jf) {
    if (jf->function) {
        // compilable function
        // create inputs/outputs for the function
        jf->inputs = (void**) calloc(1, sizeof(void*) * jf->pipeline->inputData->objects.size());
        jf->outputs = (void**) calloc(1, sizeof(void*) * jf->pipeline->outputData->objects.size());
        // set inputs
        size_t i = 0;
        for(auto it = jf->pipeline->inputData->objects.begin(); it != jf->pipeline->inputData->objects.end(); it++, i++) {
            jf->inputs[i] = it->source->data;
        }
        i = 0;
        // allocate space for the results (if the space has not yet been allocated) and setup outputs
        for (auto it = jf->pipeline->outputData->objects.begin(); it != jf->pipeline->outputData->objects.end(); it++, i++) {
            if (it->source->object->storage == NULL) {
                npy_intp elements[] = { (npy_intp) it->source->size };
                // todo: set to PyArray_EMPTY
                it->source->object->storage = (PyArrayObject*) PyArray_ZEROS(1, elements, it->source->type, 0);
                it->source->data = PyArray_DATA(it->source->object->storage);
            }
            jf->outputs[i] = it->source->data;
        }
    } else {
        // base function
        assert(jf->base);
    }
    ExecuteTask *task = (ExecuteTask*) malloc(sizeof(ExecuteTask));
    task->type = tasktype_execute;
    task->start = 0;
    task->end = jf->size;
    task->function = jf;
    jf->references++;
    ScheduleTask((Task*) task);
}

void inline 
DestroyTask(Task *task) {
    free(task);
}

void 
ScheduleTask(Task *task) {
    queue.enqueue(task);
}

void
RunThread(Thread *thread) {
    while (running) {
        Task *task;
        printf("Thread %zu waiting for task.\n", thread->index);
        //if (!queue.try_dequeue(task)) return;
        queue.wait_dequeue(task);
        printf("Thread %zu obtained task.\n", thread->index);
        if (task == NULL) {
            printf("Null.\n");
            continue;
        }
        //execute task
        switch(task->type) {
            case tasktype_compile: {
                Pipeline *pipeline = ((CompileTask*)task)->pipeline;
                printf("Compile pipeline %s\n", pipeline->name);
                if (CompilableOperation(pipeline->operation)) {
                    JITFunction *jf = CompilePipeline(pipeline, thread);
                    ((CompileTask*) task)->pipeline->function = jf;
                    if (jf != NULL) {
                        ScheduleExecution(pipeline);
                    } else {
                        printf("Failed to compile function, free semaphore\n");
                        // free the lock so the error can be reported
                        while(pipeline) {
                            printf("Semaphore %p\n", pipeline);
                            if (pipeline->semaphore) {
                                printf("Free semaphore.\n");
                                semaphore_increment(&pipeline->semaphore);
                            }
                            pipeline = pipeline->parent;
                        }
                    }
                } else {
                    printf("Perform base operation for function.\n");
                    JITFunction *jf = GenerateBaseFunction(pipeline, thread);
                    ((CompileTask*) task)->pipeline->function = jf;
                    if (jf != NULL) {
                        ScheduleExecution(pipeline);
                    } else {
                        printf("Failed to schedule function\n");
                    }
                }
                break;
            }
            case tasktype_execute: {
                printf("Execute pipeline task.\n");
                ExecuteTask *ex = (ExecuteTask*) task;
                ExecuteFunction(ex->function, ex->start, ex->end);
                JITFunctionDECREF(ex->function);
                break;
            }
            default:
                printf("Unrecognized task type.\n");
        }
        DestroyTask(task);
    }
}

void 
initialize_scheduler(void) {
    import_array();
    import_umath();
}

void 
create_threads(void) {
    //launch threads
    Thread *thread = CreateThread();
}