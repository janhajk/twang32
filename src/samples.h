#include <stdlib.h>
#include <assert.h>

// Replacement for RunningMedian library with only the features we need for this project
// and other simplifications
//
// NOTE: We always assume all samples to be filled, since the case of it being partially
// filled complicates the code a lot, but only accounts for the first few ms of the
// program starting. So we rather are wrong for these few ms, than be slow and complicated
// for 99,999% of the rest of the time the program runs.

typedef struct Samples 
{
    int values[5];
    int sorted[5];
    int index;
} Samples;

#define SAMPLE_CNT (sizeof(((Samples*)0)->values) / sizeof(((Samples*)0)->values[0]))

// make sure odd count of elements (for simplified median calculation)
static_assert(SAMPLE_CNT % 2 != 0);
// make sure values is same size as sorted
static_assert(sizeof(((Samples*)0)->values) == sizeof(((Samples*)0)->sorted));

void sample_add(Samples *s, int val)
{
    assert(s);
    assert(s->index < SAMPLE_CNT);

    s->values[s->index] = val;
    s->index++;
    if (s->index == SAMPLE_CNT)
        s->index = 0;
}

int cmp_int(const void *a, const void *b)
{
    int va = *(const int*)a;
    int vb = *(const int*)b;
    return va - vb;
}

int sample_median(Samples *s)
{
    memcpy(s->sorted, s->values, sizeof(s->values));
    qsort(s->sorted, SAMPLE_CNT, sizeof(s->values[0]), cmp_int);
    // normal program loop always adds a value before querying the median invalidating the cache 
    // if that was not the case we could check for sorted and short circuit to the result
    return s->sorted[SAMPLE_CNT / 2];
}

int sample_highest(Samples *s)
{
    int result = s->values[0];
    for (int idx = 1; idx < SAMPLE_CNT; ++idx)
    {
        if (s->values[idx] > result)
            result = s->values[idx];
    }
    return result;
}