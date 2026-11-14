#include <pthread.h>
#include <unistd.h>
#include <cstdio>
#include <string>

class Event
{
public:
    explicit Event(int number) : number(number), text("Событие №" + std::to_string(number)) {}
    int number;
    std::string text;
};

struct Monitor
{
    pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
    bool ready = false;     
    bool stopping = false;  
    Event* data = nullptr;  
};

static Monitor monitor;
static const int EVENTS_COUNT = 5;

static void* producer(void*)
{
    for (int i = 1; i <= EVENTS_COUNT; ++i)
    {
        sleep(1); 

        bool sent = false;
        while (!sent)
        {
            pthread_mutex_lock(&monitor.mutex);
            if (monitor.ready) 
            {
                pthread_mutex_unlock(&monitor.mutex);
                usleep(10000); 
                continue;
            }
            monitor.data = new Event(i);
            monitor.ready = true;
            
            printf("[Поставщик]  отправлено: %s\n", monitor.data->text.c_str());
            fflush(stdout);
            pthread_cond_signal(&monitor.condition);
            pthread_mutex_unlock(&monitor.mutex);
            sent = true;
        }
    }

    pthread_mutex_lock(&monitor.mutex);
    monitor.stopping = true;
    pthread_cond_broadcast(&monitor.condition);
    pthread_mutex_unlock(&monitor.mutex);
    return nullptr;
}

static void* consumer(void*)
{
    pthread_mutex_lock(&monitor.mutex);
    for (;;)
    {
        while (!monitor.ready && !monitor.stopping)
            pthread_cond_wait(&monitor.condition, &monitor.mutex);

        if (monitor.ready)
        {
            Event* event = monitor.data;
            monitor.data = nullptr;
            monitor.ready = false;
            printf("[Потребитель] получено:   %s\n", event->text.c_str());
            fflush(stdout);
            delete event;
        }
        else if (monitor.stopping)
            break;
    }
    pthread_mutex_unlock(&monitor.mutex);
    return nullptr;
}

int main()
{
    pthread_t producerThread, consumerThread;
    pthread_create(&consumerThread, nullptr, consumer, nullptr);
    pthread_create(&producerThread, nullptr, producer, nullptr);

    pthread_join(producerThread, nullptr);
    pthread_join(consumerThread, nullptr);

    pthread_cond_destroy(&monitor.condition);
    pthread_mutex_destroy(&monitor.mutex);
    return 0;
}