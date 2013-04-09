#define COMPUTE_ITER 1200
#define LOW_ITER 2000
#define RATIO 20

#include <cstdlib>

int main(int argc, char** argv)
{
    int arr[LOW_ITER / RATIO];
    double sum = 0.0;

    // Init array to throw off predictor
    for (int i=0; i < LOW_ITER / RATIO; i++)
        arr[i] = rand() % 10;

    // Do some compute
    for (int j=0; j < COMPUTE_ITER; j++) {
        sum += (double) j;
        if (j % 5 == 0)
            sum *= 3.34;
    }

    // This should throw off the branch predictor,
    // leading to lowish IPC.
    for (int k=0; k < LOW_ITER; k++)
        if (arr[k % (LOW_ITER / RATIO) < 5])
            sum -= 1;
        else {
            __asm__("nop; nop; nop;");
            __asm__("nop; nop; nop;");
            __asm__("nop; nop; nop;");
        }

    return (sum <= 0);
}
