#include <pybind11/pybind11.h>
#include <pybind11/numpy.h>
#ifdef _MSC_VER
#include <nmmintrin.h>
#endif
#include "sim.hpp"

namespace py = pybind11;

#ifdef _WIN64
__inline long long popcntll(unsigned long long X)
{
    return _mm_popcnt_u64(X);
}
#else
__inline long long popcntll(unsigned long long X)
{
    return __builtin_popcountll(X);
}
#endif

uint32_t py_popcount(py::array_t<unsigned long long> pyquery)
{
    auto query = pyquery.unchecked<1>();
    uint32_t qcount = 0;
    for (ssize_t i = 0; i < query.shape(0); i++)
        qcount += popcntll(query(i));
    return qcount;
}

__inline float tanimoto_coeff(uint32_t int_count, uint32_t qcount, uint32_t ocount)
{
    float coeff = 0.0;
    coeff = qcount + ocount - int_count;
    if (coeff != 0.0)
        coeff = int_count / coeff;
    return coeff;
}

__inline float tversky_coeff(uint32_t int_count, uint32_t rel_co_count, uint32_t rel_co_count2, float a, float b)
{
    float coeff = 0.0;
    coeff = int_count + a * rel_co_count + b * rel_co_count2;
    if (coeff != 0.0)
        coeff = int_count / coeff;
    return coeff;
}

__inline float substruct_coeff(uint32_t rel_co_count, uint32_t int_count)
{
    float coeff = 0.0;
    coeff = rel_co_count + int_count;
    if (coeff != 0.0)
        coeff = int_count / coeff;
    return coeff;
}

py::array_t<uint32_t> _substructure_search(py::array_t<unsigned long long> pyquery,
                                           py::array_t<unsigned long long> pydb,
                                           float threshold,
                                           float a,
                                           float b,
                                           uint8_t sim_type,
                                           uint32_t i_start,
                                           uint32_t i_end)
{
    // release the GIL
    py::gil_scoped_release release;

    auto query = pyquery.unchecked<1>();
    auto db = pydb.unchecked<2>();

    // initial similarity result array size, allocate memory for results
    uint32_t subsres_length = 256;
    auto results = new std::vector<uint32_t>(subsres_length);

    size_t qshape = query.shape(0);
    uint8_t popcntidx = qshape - 1;

    uint32_t int_count = 0;
    uint32_t rel_co_count = 0;
    float coeff = 0.0;
    uint32_t total_subs = 0;
    uint32_t i = i_start;
    while (i_end > i)
    {
        // calc count for intersection and relative complement
        for (size_t j = 1; j < popcntidx; j++)
        {
            int_count += popcntll(query(j) & db(i, j));
            rel_co_count += popcntll(query(j) & ~db(i, j));
        }
        // calc optimised tversky with a=1, b=0
        coeff = substruct_coeff(rel_co_count, int_count);

        if (coeff == threshold)
        {
            (*results)[total_subs] = db(i, 0);
            total_subs += 1;
        }
        if (total_subs == subsres_length)
        {
            subsres_length *= 1.12;
            results->resize(subsres_length);
        }
        //  reset values for next fp
        int_count = 0;
        rel_co_count = 0;
        i++;
    }
    results->resize(total_subs);

    // acquire the GIL
    py::gil_scoped_acquire acquire;

    // python object that will free the memory when destroyed
    auto capsule = py::capsule(results, [](void *results) {
        delete reinterpret_cast<std::vector<uint32_t> *>(results);
    });
    return py::array_t<uint32_t>(results->size(), results->data(), capsule);
}

bool cmp(const Result &l, const Result &r) { return l.coeff > r.coeff; }

void sort_results(py::array_t<Result> pyres)
{
    py::buffer_info buf = pyres.request();
    Result *ptr = (Result *)buf.ptr;
    std::sort(&ptr[0], &ptr[buf.shape[0]], cmp);
}

py::array_t<Result> _similarity_search(py::array_t<unsigned long long> pyquery,
                                       py::array_t<unsigned long long> pydb,
                                       float threshold,
                                       float a,
                                       float b,
                                       uint8_t sim_type,
                                       uint32_t i_start,
                                       uint32_t i_end)
{
    // release the GIL
    py::gil_scoped_release release;

    // direct access to np arrays without checks
    auto query = pyquery.unchecked<1>();
    auto db = pydb.unchecked<2>();

    // initial similarity result array size, allocate memory for results
    uint32_t simres_length = 256;
    auto results = new std::vector<Result>(simres_length);

    size_t qshape = query.shape(0);
    uint8_t popcntidx = qshape - 1;

    uint32_t int_count = 0;
    uint32_t rel_co_count = 0;
    uint32_t rel_co_count2 = 0;
    uint32_t total_sims = 0;
    float coeff = 0.0;
    uint32_t i = i_start;
    while (i_end > i)
    {
        for (size_t j = 1; j < popcntidx; j++)
        {
            int_count += popcntll(query(j) & db(i, j));
        }

        if (sim_type == 1) // tversky
        {
            for (size_t j = 1; j < popcntidx; j++)
            {
                rel_co_count += popcntll(query(j) & ~db(i, j));
                rel_co_count2 += popcntll(db(i, j) & ~query(j));
            }
            coeff = tversky_coeff(int_count, rel_co_count, rel_co_count2, a, b);
            rel_co_count = 0;
            rel_co_count2 = 0;
        }
        else
        {
            coeff = tanimoto_coeff(int_count, query(popcntidx), db(i, popcntidx));
        }

        if (coeff >= threshold)
        {
            (*results)[total_sims].mol_id = db(i, 0);
            (*results)[total_sims].coeff = coeff;
            total_sims += 1;
        }
        if (total_sims == simres_length)
        {
            // reallocate memory
            simres_length *= 1.12;
            results->resize(simres_length);
        }
        int_count = 0;
        i++;
    }
    // set final size and sort
    results->resize(total_sims);
    std::sort(results->begin(), results->end(), cmp);

    // acquire the GIL
    py::gil_scoped_acquire acquire;

    // python object that will free the memory when destroyed
    auto capsule = py::capsule(results, [](void *results) {
        delete reinterpret_cast<std::vector<Result> *>(results);
    });
    return py::array_t<Result>(results->size(), results->data(), capsule);
}
