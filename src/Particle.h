#include "Arduino.h"

#define USE_GRAVITY 0  // 0/1 use gravity (LED strip going up wall)
#define BEND_POINT 550 // 0/1000 point at which the LED strip goes up the wall

class Particle
{
public:
    void Spawn(int pos);
    void Tick();
    void Kill();
    bool Alive();
    int _pos;
    int _power;

private:
    int _life;
    int _alive;
    int _sp;
};

void Particle::Spawn(int pos)
{
    _pos = pos;
    _sp = random(-200, 200);
    _power = 255;
    _alive = 1;
    _life = 220 - abs(_sp);
}

void Particle::Tick()
{
    if (_alive)
    {
        _life++;
        if (_sp > 0)
        {
            _sp -= _life / 10;
        }
        else
        {
            _sp += _life / 10;
        }
        if (USE_GRAVITY && _pos > BEND_POINT)
            _sp -= 10;
        _power = 100 - _life;
        if (_power <= 0)
        {
            Kill();
        }
        else
        {
            _pos += _sp / 7.0;
            if (_pos > 1000)
            {
                _pos = 1000;
                _sp = 0 - (_sp / 2);
            }
            else if (_pos < 0)
            {
                _pos = 0;
                _sp = 0 - (_sp / 2);
            }
        }
    }
}

bool Particle::Alive()
{
    return _alive;
}

void Particle::Kill()
{
    _alive = 0;
}
