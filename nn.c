/* RPROP Neural Networks implementation
 * See: http://deeplearning.cs.cmu.edu/pdfs/Rprop.pdf
 *
 * Copyright (c) 2003-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Disque nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <string.h>

#ifdef USE_AVX
#include <xmmintrin.h>
#include <pmmintrin.h>
#include <immintrin.h>
#endif

#include "nn.h"

/* Node Transfer Function */
float sigmoid(float x) {
    return (float)1/(1+exp(-x));
}

float relu(float x) {
    return (x > 0) ? x : 0;
}

/* Reset layer data to zero-units */
void AnnResetLayer(struct AnnLayer *layer) {
    layer->units = 0;
    layer->output = NULL;
    layer->error = NULL;
    layer->weight = NULL;
    layer->gradient = NULL;
    layer->pgradient = NULL;
    layer->delta = NULL;
    layer->sgradient = NULL;
}

/* Allocate and return an initialized N-layers network */
struct Ann *AnnAlloc(int layers) {
    struct Ann *net;
    int i;

    /* Alloc the net structure */
    if ((net = malloc(sizeof(*net))) == NULL)
        return NULL;
    /* Alloc layers */
    if ((net->layer = malloc(sizeof(struct AnnLayer)*layers)) == NULL) {
        free(net);
        return NULL;
    }
    net->layers = layers;
    net->flags = 0;
    net->rprop_nminus = DEFAULT_RPROP_NMINUS;
    net->rprop_nplus = DEFAULT_RPROP_NPLUS;
    net->rprop_maxupdate = DEFAULT_RPROP_MAXUPDATE;
    net->rprop_minupdate = DEFAULT_RPROP_MINUPDATE;
    /* Init layers */
    for (i = 0; i < layers; i++)
        AnnResetLayer(&net->layer[i]);
    return net;
}

/* Free a single layer */
void AnnFreeLayer(struct AnnLayer *layer)
{
    free(layer->output);
    free(layer->error);
    free(layer->weight);
    free(layer->gradient);
    free(layer->pgradient);
    free(layer->delta);
    free(layer->sgradient);
    AnnResetLayer(layer);
}

/* Free the target net */
void AnnFree(struct Ann *net)
{
    int i;

    /* Free layer data */
    for (i = 0; i < net->layers; i++) AnnFreeLayer(&net->layer[i]);
    /* Free allocated layers structures */
    free(net->layer);
    /* And the main structure itself */
    free(net);
}

/* Init a layer of the net with the specified number of units.
 * Return non-zero on out of memory. */
int AnnInitLayer(struct Ann *net, int i, int units, int bias) {
    if (bias) units++; /* Take count of the bias unit */
    net->layer[i].output = malloc(sizeof(float)*units);
    net->layer[i].error = malloc(sizeof(float)*units);
    if (i) { /* not for output layer */
        net->layer[i].weight =
            malloc(sizeof(float)*units*net->layer[i-1].units);
        net->layer[i].gradient =
            malloc(sizeof(float)*units*net->layer[i-1].units);
        net->layer[i].pgradient =
            malloc(sizeof(float)*units*net->layer[i-1].units);
        net->layer[i].delta =
            malloc(sizeof(float)*units*net->layer[i-1].units);
        net->layer[i].sgradient =
            malloc(sizeof(float)*units*net->layer[i-1].units);
    }
    net->layer[i].units = units;
    /* Check for out of memory conditions */
    if (net->layer[i].output == NULL ||
        net->layer[i].error == NULL ||
        (i && net->layer[i].weight == NULL) ||
        (i && net->layer[i].gradient == NULL) ||
        (i && net->layer[i].pgradient == NULL) ||
        (i && net->layer[i].sgradient == NULL) ||
        (i && net->layer[i].delta == NULL))
    {
        AnnFreeLayer(&net->layer[i]);
        AnnResetLayer(&net->layer[i]);
        return 1;
    }
    /* Set all the values to zero */
    memset(net->layer[i].output, 0, sizeof(float)*units);
    memset(net->layer[i].error, 0, sizeof(float)*units);
    if (i) {
        memset(net->layer[i].weight, 0,
            sizeof(float)*units*net->layer[i-1].units);
        memset(net->layer[i].gradient, 0,
            sizeof(float)*units*net->layer[i-1].units);
        memset(net->layer[i].pgradient, 0,
            sizeof(float)*units*net->layer[i-1].units);
        memset(net->layer[i].delta, 0,
            sizeof(float)*units*net->layer[i-1].units);
        memset(net->layer[i].sgradient, 0,
            sizeof(float)*units*net->layer[i-1].units);
    }
    /* Set the bias unit output to 1 */
    if (bias) net->layer[i].output[units-1] = 1;
    return 0;
}

/* Clone a network. On out of memory NULL is returned. */
struct Ann *AnnClone(struct Ann* net) {
    struct Ann* copy;
    int j;

    if ((copy = AnnAlloc(LAYERS(net))) == NULL) return NULL;
    for (j = 0; j < LAYERS(net); j++) {
        struct AnnLayer *ldst, *lsrc;
        int units = UNITS(net,j);
        int bias = j > 0;
        if (AnnInitLayer(copy, j, units-bias, bias)) {
            AnnFree(copy);
            return NULL;
        }
        lsrc = &net->layer[j];
        ldst = &copy->layer[j];
        if (lsrc->output)
            memcpy(ldst->output, lsrc->output, sizeof(float)*units);
        if (lsrc->error)
            memcpy(ldst->error, lsrc->error, sizeof(float)*units);
        if (j) {
            int weights = WEIGHTS(net,j);
            if (lsrc->weight)
                memcpy(ldst->weight, lsrc->weight, sizeof(float)*weights);
            if (lsrc->gradient)
                memcpy(ldst->gradient, lsrc->gradient, sizeof(float)*weights);
            if (lsrc->pgradient)
                memcpy(ldst->pgradient, lsrc->pgradient, sizeof(float)*weights);
            if (lsrc->delta)
                memcpy(ldst->delta, lsrc->delta, sizeof(float)*weights);
            if (lsrc->sgradient)
                memcpy(ldst->sgradient, lsrc->sgradient, sizeof(float)*weights);
        }
    }
    copy->rprop_nminus = net->rprop_nminus;
    copy->rprop_nplus = net->rprop_nplus;
    copy->rprop_maxupdate = net->rprop_maxupdate;
    copy->rprop_minupdate = net->rprop_minupdate;
    copy->flags = net->flags;
    return copy;
}

/* Create a N-layer input/hidden/output net.
 * The units array should specify the number of
 * units in every layer from the output to the input layer. */
struct Ann *AnnCreateNet(int layers, int *units) {
    struct Ann *net;
    int i;

    if ((net = AnnAlloc(layers)) == NULL) return NULL;
    for (i = 0; i < layers; i++) {
        if (AnnInitLayer(net, i, units[i], i > 0)) {
            AnnFree(net);
            return NULL;
        }
    }
    AnnSetRandomWeights(net);
    AnnSetDeltas(net, RPROP_INITIAL_DELTA);
    LEARN_RATE(net) = DEFAULT_LEARN_RATE;
    return net;
}

/* Return the total number of weights this NN has. */
size_t AnnCountWeights(struct Ann *net) {
    size_t weights = 0;
    for (int i = net->layers-1; i > 0; i--) {
        int nextunits = net->layer[i-1].units;
        int units = net->layer[i].units;
        if (i > 1) nextunits--; /* we don't output on bias units */
        weights += units*nextunits;
    }
    return weights;
}

/* Create a 4-layer input/hidden/output net */
struct Ann *AnnCreateNet4(int iunits, int hunits, int hunits2, int ounits) {
    int units[4];

    units[0] = ounits;
    units[1] = hunits2;
    units[2] = hunits;
    units[3] = iunits;
    return AnnCreateNet(4, units);
}

/* Create a 3-layer input/hidden/output net */
struct Ann *AnnCreateNet3(int iunits, int hunits, int ounits) {
    int units[3];

    units[0] = ounits;
    units[1] = hunits;
    units[2] = iunits;
    return AnnCreateNet(3, units);
}


/* Create a 2-layer "linear" network. */
struct Ann *AnnCreateNet2(int iunits, int ounits) {
    int units[2];

    units[0] = ounits;
    units[1] = iunits;
    return AnnCreateNet(2, units);
}

/* Simulate the net one time. */
#ifdef USE_AVX
/* Provided to stack overflow by user Marat Dukhan. */
float avx_horizontal_sum(__m256 x) {
    // hiQuad = ( x7, x6, x5, x4 )
    const __m128 hiQuad = _mm256_extractf128_ps(x, 1);
    // loQuad = ( x3, x2, x1, x0 )
    const __m128 loQuad = _mm256_castps256_ps128(x);
    // sumQuad = ( x3 + x7, x2 + x6, x1 + x5, x0 + x4 )
    const __m128 sumQuad = _mm_add_ps(loQuad, hiQuad);
    // loDual = ( -, -, x1 + x5, x0 + x4 )
    const __m128 loDual = sumQuad;
    // hiDual = ( -, -, x3 + x7, x2 + x6 )
    const __m128 hiDual = _mm_movehl_ps(sumQuad, sumQuad);
    // sumDual = ( -, -, x1 + x3 + x5 + x7, x0 + x2 + x4 + x6 )
    const __m128 sumDual = _mm_add_ps(loDual, hiDual);
    // lo = ( -, -, -, x0 + x2 + x4 + x6 )
    const __m128 lo = sumDual;
    // hi = ( -, -, -, x1 + x3 + x5 + x7 )
    const __m128 hi = _mm_shuffle_ps(sumDual, sumDual, 0x1);
    // sum = ( -, -, -, x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 )
    const __m128 sum = _mm_add_ss(lo, hi);
    return _mm_cvtss_f32(sum);
}
#endif

void AnnSimulate(struct Ann *net) {
    int i, j, k;

    for (i = net->layers-1; i > 0; i--) {
        int nextunits = net->layer[i-1].units;
        int units = net->layer[i].units;
        if (i > 1) nextunits--; /* dont output on bias units */
        for (j = 0; j < nextunits; j++) {
            float A = 0; /* Activation final value. */
            float *w = net->layer[i].weight + j*units;
            float *o = net->layer[i].output;

            k = 0;

#ifdef USE_AVX
            int psteps = units/8;
            for (int x = 0; x < psteps; x++) {
                __m256 weights = _mm256_loadu_ps(w);
                __m256 outputs = _mm256_loadu_ps(o);
                __m256 prod = _mm256_mul_ps(weights,outputs);
                A += avx_horizontal_sum(prod);
                w += 8;
                o += 8;
            }
            k += 8*psteps;
#endif

            /* Handle final piece shorter than 16 bytes. */
            for (; k < units; k++) {
                float W = *w++;
                float O = *o++;
                A += W*O;
            }
            OUTPUT(net, i-1, j) = sigmoid(A);
        }
    }
}

/* Create a Tcl procedure that simulates the neural network */
void Ann2Tcl(struct Ann *net) {
    int i, j, k;

    printf("proc ann input {\n");
    printf("    set output {");
    for (i = 0; i < OUTPUT_UNITS(net); i++) {
        printf("0 ");
    }
    printf("}\n");
    for (i = net->layers-1; i > 0; i--) {
        int nextunits = net->layer[i-1].units;
        int units = net->layer[i].units;
        if (i > 1) nextunits--; /* dont output on bias units */
        for (j = 0; j < nextunits; j++) {
            float W;
            if (i == 1) {
                printf("    lset output %d ", j);
            } else {
                printf("    set O_%d_%d", i-1, j);
            }
            printf(" [expr { \\\n");
            for (k = 0; k < units; k++) {
                W = WEIGHT(net, i, k, j);
                if (i > 1 && k == units-1) {
                    printf("        (%.9f)", W);
                } else if (i == net->layers-1) {
                    printf("        (%.9f*[lindex $input %d])", W, k);
                } else {
                    printf("        (%.9f*$O_%d_%d)", W, i, k);
                }
                if ((k+1) < units) printf("+ \\\n");
            }
            printf("}]\n");
            if (i == 1) {
                printf("    lset output %d [expr {1/(1+exp(-[lindex $output %d]))}]\n", j, j);
            } else {
                printf("    lset O_%d_%d [expr {1/(1+exp(-$O_%d_%d))}]\n", i-1, j, i-1, j);
            }
        }
    }
    printf("    return $output\n");
    printf("}\n");
}

/* Print a network representation */
void AnnPrint(struct Ann *net) {
    int i, j, k;

    for (i = 0; i < LAYERS(net); i++) {
        char *layertype = "Hidden";
        if (i == 0) layertype = "Output";
        if (i == LAYERS(net)-1) layertype = "Input";
        printf("%s layer %d, units %d\n", layertype, i, UNITS(net,i));
        if (i) {
            /* Don't compute the bias unit as a target. */
            int targets = UNITS(net,i-1) - (i-1>0);
            /* Weights */
            printf("\tW");
            for (j = 0; j < UNITS(net, i); j++) {
                printf("(");
                for (k = 0; k < targets; k++) {
                    printf("%f", WEIGHT(net,i,j,k));
                    if (k != targets-1) printf(" ");
                }
                printf(") ");
            }
            printf("\n");
            /* Gradients */
            printf("\tg");
            for (j = 0; j < UNITS(net, i); j++) {
                printf("[");
                for (k = 0; k < targets; k++) {
                    printf("%f", GRADIENT(net,i,j,k));
                    if (k != targets-1) printf(" ");
                }
                printf("] ");
            }
            printf("\n");
            /* SGradients */
            printf("\tG");
            for (j = 0; j < UNITS(net, i); j++) {
                printf("[");
                for (k = 0; k < targets; k++) {
                    printf("%f", SGRADIENT(net,i,j,k));
                    if (k != targets-1) printf(" ");
                }
                printf("] ");
            }
            printf("\n");
            /* Gradients at t-1 */
            printf("\tP");
            for (j = 0; j < UNITS(net, i); j++) {
                printf("[");
                for (k = 0; k < targets; k++) {
                    printf("%f", PGRADIENT(net,i,j,k));
                    if (k != targets-1) printf(" ");
                }
                printf("] ");
            }
            printf("\n");
            /* Delta */
            printf("\tD");
            for (j = 0; j < UNITS(net, i); j++) {
                printf("|");
                for (k = 0; k < targets; k++) {
                    printf("%f", DELTA(net,i,j,k));
                    if (k != targets-1) printf(" ");
                }
                printf("| ");
            }
            printf("\n");
        }
        for (j = 0; j < UNITS(net,i); j++) {
            printf("\tO: %f ", OUTPUT(net,i,j));
        }
        printf("\n");
        printf("\tE /");
        for (j = 0; j < UNITS(net,i); j++) {
            printf("%f ", ERROR(net,i,j));
        }
        printf("/\n");
    }
}

/* Calcuate the global error of the net. This is just the
 * Root Mean Square (RMS) error, which is half the sum of the squared
 * errors. */
float AnnGlobalError(struct Ann *net, float *desired) {
    float e, t;
    int i, outputs = OUTPUT_UNITS(net);

    e = 0;
    for (i = 0; i < outputs; i++) {
        t = desired[i] - OUTPUT_NODE(net,i);
        e += t*t; /* No need for fabs(t), t*t will always be positive. */
    }
    return .5*e;
}

/* Set the network input */
void AnnSetInput(struct Ann *net, float *input)
{
    int i, inputs = INPUT_UNITS(net);

    for (i = 0; i < inputs; i++) INPUT_NODE(net,i) = input[i];
}

/* Simulate the net, and return the global error */
float AnnSimulateError(struct Ann *net, float *input, float *desired) {
    AnnSetInput(net, input);
    AnnSimulate(net);
    return AnnGlobalError(net, desired);
}

/* Compute the error vector y-t in the output unit. This error depends
 * on the loss function we use. */
void AnnCalculateOutputError(struct Ann *net, float *desired) {
    int units = OUTPUT_UNITS(net);
    float factor = (float)2/units;
    for (int j = 0; j < units; j++) {
        net->layer[0].error[j] =
            factor * (net->layer[0].output[j] - desired[j]);
    }
}

/* Calculate gradients with a trivial and slow algorithm, this
 * is useful to check that the real implementation is working
 * well, comparing the results.
 *
 * The algorithm used is: to compute the error function in two
 * points (E1, with the real weight, and E2 with the weight W = W + 0.1),
 * than the approximation of the gradient is G = (E2-E1)/0.1. */
#define GTRIVIAL_DELTA 0.001
void AnnCalculateGradientsTrivial(struct Ann *net, float *desired) {
    int j, i, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        for (i = 0; i < weights; i++) {
            float t, e1, e2;

            /* Calculate the value of the error function
             * in this point. */
            AnnSimulate(net);
            e1 = AnnGlobalError(net, desired);
            t = net->layer[j].weight[i];
            /* Calculate the error a bit on the right */
            net->layer[j].weight[i] += GTRIVIAL_DELTA;
            AnnSimulate(net);
            e2 = AnnGlobalError(net, desired);
            /* Restore the original weight */
            net->layer[j].weight[i] = t;
            /* Calculate the gradient */
            net->layer[j].gradient[i] = (e2-e1)/GTRIVIAL_DELTA;
        }
    }
}

/* Calculate gradients using the back propagation algorithm */
void AnnCalculateGradients(struct Ann *net, float *desired) {
    int j, layers = LAYERS(net)-1;

    /* Populate the error vector net->layer[0]->error according
     * to the loss function. */
    AnnCalculateOutputError(net,desired);

    /* Back-propagate the error and compute the gradient
     * for every weight in the net. */
    for (j = 0; j < layers; j++) {
        struct AnnLayer *layer = &net->layer[j];
        struct AnnLayer *prev_layer = &net->layer[j+1];
        int i, units = layer->units;
        int prevunits = prev_layer->units;

        /* Skip bias units, they have no connections with the previous
         * layers. */
        if (j > 1) units--;
        /* Reset the next layer errors array */
        for (i = 0; i < prevunits; i++) prev_layer->error[i] = 0;
        /* For every node in this layer ... */
        for (i = 0; i < units; i++) {
            float error_signal, ei, oi, derivative;
            int k;

            /* Compute gradient. */
            ei = layer->error[i];
            oi = layer->output[i];

            /* Common derivatives:
             *
             * identity: 1
             * sigmoid: oi*(1-oi)
             * softmax: oi*(1-oi)
             * tanh:    (1-oi)*(1+oi), that's 1-(oi*oi)
             * relu:    (oi > 0) ? 1 : 0
             */
            derivative = oi*(1-oi);
            error_signal = ei*derivative;

            /* For every weight between this node and
             * the previous layer's nodes: */
            float *g = prev_layer->gradient + i*prevunits;
            float *w = prev_layer->weight + i*prevunits;
            float *o = prev_layer->output;
            float *e = prev_layer->error;

            /* 1. Calculate the gradient */
            k = 0;
#ifdef USE_AVX
            __m256 es = _mm256_set1_ps(error_signal);

            int psteps = prevunits/8;
            for (int x = 0; x < psteps; x++) {
                __m256 outputs = _mm256_loadu_ps(o);
                __m256 gradients = _mm256_mul_ps(es,outputs);
                _mm256_storeu_ps(g,gradients);
                o += 8;
                g += 8;
            }
            k += 8*psteps;
#endif
            for (; k < prevunits; k++) *g++ = error_signal*(*o++);

            /* 2. And back-propagate the error to the previous layer */
            k = 0;
#ifdef USE_AVX
            psteps = prevunits/8;
            for (int x = 0; x < psteps; x++) {
                __m256 weights = _mm256_loadu_ps(w);
                __m256 errors = _mm256_loadu_ps(e);
                __m256 prod = _mm256_fmadd_ps(es,weights,errors);
                _mm256_storeu_ps(e,prod);
                e += 8;
                w += 8;
            }
            k += 8*psteps;
#endif
            for (; k < prevunits; k++) {
                *e += error_signal * (*w);
                e++;
                w++;
            }
        }
    }
}

/* Set the delta values of the net to a given value */
void AnnSetDeltas(struct Ann *net, float val) {
    int j, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        int i;

        for (i = 0; i < weights; i++) net->layer[j].delta[i] = val;
    }
}

/* Set the sgradient values to zero */
void AnnResetSgradient(struct Ann *net) {
    int j, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        memset(net->layer[j].sgradient, 0, sizeof(float)*weights);
    }
}

/* Set random weights in the range -0.05,+0.05 */
void AnnSetRandomWeights(struct Ann *net) {
    int i, j, k;

    for (i = 1; i < LAYERS(net); i++) {
        for (k = 0; k < UNITS(net, i-1); k++) {
            for (j = 0; j < UNITS(net, i); j++) {
                WEIGHT(net,i,j,k) = -0.05+.1*(rand()/(RAND_MAX+1.0));
            }
        }
    }
}

/* Scale the net weights of the given factor */
void AnnScaleWeights(struct Ann *net, float factor) {
    int j, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        int i;

        for (i = 0; i < weights; i++)
            net->layer[j].weight[i] *= factor;
    }
}

/* Update the sgradient, that's the sum of the weight's gradient for every
 * element of the training set. This is used for the RPROP algorithm
 * that works with the sign of the derivative for the whole set. */
void AnnUpdateSgradient(struct Ann *net) {
    int j, i, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        /* In theory this is a good target for SSE "ADDPS" instructions,
         * however modern compilers figure out this automatically. */
        for (i = 0; i < weights; i++)
            net->layer[j].sgradient[i] += net->layer[j].gradient[i];
    }
}

/* Helper function for RPROP, returns -1 if n < 0, +1 if n > 0, 0 if n == 0 */
float sign(float n) {
    if (n > 0) return +1;
    if (n < 0) return -1;
    return 0;
}

/* The core of the RPROP algorithm.
 *
 * Note that:
 * sgradient is the set-wise gradient.
 * delta is the per-weight update value. */
void AnnAdjustWeightsResilientBP(struct Ann *net) {
    int j, i, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1) - (j-1>0);
        for (i = 0; i < weights; i++) {
            float t = net->layer[j].pgradient[i] *
                       net->layer[j].sgradient[i];
            float delta = net->layer[j].delta[i];

            if (t > 0) {
                delta = MIN(delta*RPROP_NPLUS(net),RPROP_MAXUPDATE(net));
                float wdelta = -sign(net->layer[j].sgradient[i]) * delta;
                net->layer[j].weight[i] += wdelta;
                net->layer[j].delta[i] = delta;
                net->layer[j].pgradient[i] = net->layer[j].sgradient[i];
            } else if (t < 0) {
                float past_wdelta = -sign(net->layer[j].pgradient[i]) * delta;
                delta = MAX(delta*RPROP_NMINUS(net),RPROP_MINUPDATE(net));
                net->layer[j].weight[i] -= past_wdelta;
                net->layer[j].delta[i] = delta;
                net->layer[j].pgradient[i] = 0;
            } else { /* t == 0 */
                float wdelta = -sign(net->layer[j].sgradient[i]) * delta;
                net->layer[j].weight[i] += wdelta;
                net->layer[j].pgradient[i] = net->layer[j].sgradient[i];
            }
        }
    }
}

/* Resilient Backpropagation Epoch */
float AnnResilientBPEpoch(struct Ann *net, float *input, float *desired, int setlen) {
    float error = 0;
    int j, inputs = INPUT_UNITS(net), outputs = OUTPUT_UNITS(net);

    AnnResetSgradient(net);
    for (j = 0; j < setlen; j++) {
        error += AnnSimulateError(net, input, desired);
        AnnCalculateGradients(net, desired);
        AnnUpdateSgradient(net);
        input += inputs;
        desired += outputs;
    }
    AnnAdjustWeightsResilientBP(net);
    return error / setlen;
}

/* Update the deltas using the gradient descend algorithm.
 * Gradients should be already computed with AnnCalculateGraidents(). */
void AnnUpdateDeltasGD(struct Ann *net) {
    int j, i, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        for (i = 0; i < weights; i++)
            net->layer[j].delta[i] += net->layer[j].gradient[i];
    }
}

/* Adjust net weights using the (already) calculated deltas. */
void AnnAdjustWeights(struct Ann *net, int setlen) {
    int j, i, layers = LAYERS(net);

    for (j = 1; j < layers; j++) {
        int units = UNITS(net, j);
        int weights = units * UNITS(net,j-1);
        for (i = 0; i < weights; i++) {
            net->layer[j].weight[i] -= LEARN_RATE(net)/setlen*net->layer[j].delta[i];
        }
    }
}

/* Gradient Descend training */
float AnnGDEpoch(struct Ann *net, float *input, float *desidered, int setlen) {
    float error = 0;
    int j, inputs = INPUT_UNITS(net), outputs = OUTPUT_UNITS(net);

    for (j = 0; j < setlen; j++) {
        AnnSetDeltas(net, 0);
        error += AnnSimulateError(net, input, desidered);
        AnnCalculateGradients(net, desidered);
        AnnUpdateDeltasGD(net);
        input += inputs;
        desidered += outputs;
        AnnAdjustWeights(net,setlen);
    }
    return error / setlen;
}

/* This function, called after AnnSimulate(), will return 1 if there is
 * an error in the detected class (compared to the desired output),
 * othewise 0 is returned. */
int AnnTestClassError(struct Ann *net, float *desired) {
    int i, outputs = OUTPUT_UNITS(net);
    int classid, outid;
    float max = 0;

    /* Get the class ID from the test dataset output. */
    classid = 0;
    for (i = 0; i < outputs; i++)
        if (desired[i] == 1) break;
    classid = i;

    /* Get the network classification. */
    max = OUTPUT_NODE(net,0);
    outid = 0;
    for (i = 1; i < outputs; i++) {
        float o = OUTPUT_NODE(net,i);
        if (o > max) {
            outid = i;
            max = o;
        }
    }
    return outid != classid;
}

/* Simulate the entire test dataset with the neural network and returns the
 * average error of all the entries tested. */
void AnnTestError(struct Ann *net, float *input, float *desired, int setlen, float *avgerr, float *classerr) {
    float error = 0;
    int j, inputs = INPUT_UNITS(net), outputs = OUTPUT_UNITS(net);
    int class_errors = 0;

    for (j = 0; j < setlen; j++) {
        error += AnnSimulateError(net, input, desired);
        if (classerr)
            class_errors += AnnTestClassError(net, desired);
        input += inputs;
        desired += outputs;
    }
    if (avgerr) *avgerr = error/setlen;
    if (classerr) *classerr = (float)class_errors*100/setlen;
}

/* Train the net */
float AnnTrain(struct Ann *net, float *input, float *desired, float maxerr, int maxepochs, int setlen, int algo) {
    int i = 0;
    float e = maxerr+1;

    while (i++ < maxepochs && e >= maxerr) {
        if (algo == NN_ALGO_BPROP) {
            e = AnnResilientBPEpoch(net, input, desired, setlen);
        } else if (algo == NN_ALGO_GD) {
            e = AnnGDEpoch(net, input, desired, setlen);
        }
    }
    return e;
}
