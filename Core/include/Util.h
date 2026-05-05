#pragma once
#include <random>
#include <vector>
#include <algorithm>

inline std::mt19937 generator(std::random_device{}());
inline const double CASH_PRECISION = 0.01;

/* Random generator will use the given seed
*****************IMPORTANT**********************
MUST BE CALLED BEFORE ANY USAGE OF THE GENERATOR TO SET CORRECTLY
*/
inline void setSeed(unsigned int seed) {
    generator.seed(seed);
}
/* Random generator will use a random seed
*****************IMPORTANT**********************
MUST BE CALLED BEFORE ANY USAGE OF THE GENERATOR TO SET CORRECTLY
*/
inline void setRandomSeed() {
    generator.seed(std::random_device{}());
}

/* Randomly shuffle a vector */
template <typename T>
inline void shuffleVector(std::vector<T>& vec) {
    std::shuffle(vec.begin(), vec.end(), generator);
}
/* Get a value from beta distribution */
inline double sampleBeta(double alpha, double beta) {
    std::gamma_distribution<double> ga(alpha, 1.0);
    std::gamma_distribution<double> gb(beta, 1.0);
    double x = ga(generator);
    double y = gb(generator);
    return x / (x + y);
}
/*
round to the desired precision

roundTo(10.0078, 0.001) = 10.008
roundTo(10.0078, 0.01) = 10.01
roundTo(10.0078, 0.1) = 10.0 or 10
roundTo(10.0078, 1) = 10
*/
inline double roundTo(double val, double precision = 0.01) {
    return std::round(val / precision) * precision;
}
inline int randomInt(int min, int max) {
    std::uniform_int_distribution<int> dist(min, max);
    return dist(generator);
}
inline unsigned int randomUInt(unsigned int min, unsigned int max) {
    std::uniform_int_distribution<unsigned int> dist(min, max);
    return dist(generator);
}
inline double randomDouble(double min, double max) {
    std::uniform_real_distribution<double> unif(min, max);
    return unif(generator);
}