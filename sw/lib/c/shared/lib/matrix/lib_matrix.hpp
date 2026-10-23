#pragma once

/* Includes */
#include "lib_types.h"
#include "lib_utils.h"


/* Defines */

/* Typedefs */

template <typename T, size_t R, size_t C>
struct Mat
{
    T e[R][C];

    [[nodiscard]] constexpr Mat operator+(const Mat & rhs) const
    {
        Mat out{};
        for (size_t i = 0U; i < R; i++)
        {
            for (size_t j = 0U; j < C; j++)
            {
                out.e[i][j] = e[i][j] + rhs.e[i][j];
            }
        }
        return out;
    }
    [[nodiscard]] constexpr Mat operator-(const Mat & rhs) const
    {
        Mat out{};
        for (size_t i = 0U; i < R; i++)
        {
            for (size_t j = 0U; j < C; j++)
            {
                out.e[i][j] = e[i][j] - rhs.e[i][j];
            }
        }
        return out;
    }

    template <size_t K>
    [[nodiscard]] constexpr Mat<T, R, K> operator*(const Mat<T, C, K> & rhs) const
    {
        Mat<T, R, K> out{};
        for (size_t i = 0U; i < R; i++)
        {
            for (size_t j = 0U; j < K; j++)
            {
                for (size_t k = 0U; k < C; k++)
                {
                    out.e[i][j] += e[i][k] * rhs.e[k][j];
                }
            }
        }
        return out;
    }

    [[nodiscard]] constexpr Mat<T, R, C> operator*(const T a) const
    {
        Mat<T, R, C> out{};
        for (size_t i = 0U; i < R; i++)
        {
            for (size_t j = 0U; j < C; j++)
            {
                out.e[i][j] = e[i][j] * a;
            }
        }
        return out;
    }

    [[nodiscard]] constexpr Mat<T, C, R> transpose() const
    {
        Mat<T, C, R> out{};
        for (size_t i = 0U; i < R; i++)
        {
            for (size_t j = 0U; j < C; j++)
            {
                out.e[j][i] = e[i][j];
            }
        }
        return out;
    }
};

/* Public Function Declarations  */

