#define NUM_ITER 1000
#define RATIO 20

#include <cstdlib>

int main(int argc, char** argv)
{
    int arr[NUM_ITER / RATIO];
    double sum = 0.0;

    // Init array to throw off predictor
    for (int i=0; i < NUM_ITER / RATIO; i++)
        arr[i] = rand() % 10;

    // Do some compute
    for (int j=0; j < NUM_ITER; j++) {
        sum += (double) j;
        if (j % 5 == 0)
            sum *= 3.34;
    }

    // This should throw off the branch predictor,
    // leading to lowish IPC.
    for (int k=0; k < NUM_ITER; k++)
        if (arr[k % (NUM_ITER / RATIO) < 5])
            sum -= 1;

    return (sum > 0);
}
