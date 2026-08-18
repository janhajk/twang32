#include "Arduino.h"

class Spawner
{
public:
    void Spawn(int pos, int rate_ms, int speed, int dir, int startOffset_ms);
    void Kill();
    int Alive();
    int _pos;
    int _rate;
    int _sp;
    int _dir;
    unsigned long _lastSpawned;
    int _delayOnce;

private:
    int _alive;
};

void Spawner::Spawn(int pos, int rate_ms, int speed, int dir, int startOffset_ms)
{
    _pos = pos;
    _rate = rate_ms;
    _sp = speed;
    _dir = dir;
    _lastSpawned = millis();
    _delayOnce = startOffset_ms;
    _alive = 1;
}

void Spawner::Kill()
{
    _alive = 0;
}

int Spawner::Alive()
{
    return _alive;
}
