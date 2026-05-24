# Fine-Grained Storage Study for DUNE


This repository contains a prototype implementation and performance study of a fine-grained data storage infrastructure for the DUNE FORM framework.

It explores a new I/O model designed to support large-scale data processing with independently managed sub-records stored in separate containers. The system enables eager writing of sub-records without requiring global synchronization, while introducing controlled synchronization mechanisms at read time when multiple sub-records must be accessed simultaneously.

The goal is to evaluate storage layouts, quantify I/O performance under different access patterns, and develop optimization strategies such as caching and metadata-aware access planning.

---

## Project Context

This project is part of the **Google Summer of Code (GSoC)** initiative under the HSF ecosystem.

Reference proposal:
[GSoC 2026 Project Description](https://hepsoftwarefoundation.org/gsoc/2026/proposal_DUNE_FORM.html)


---

## Key Objectives

- Study fine-grained data placement in independent containers
- Benchmark read performance in non-synchronized storage layouts
- Analyze performance bottlenecks under different I/O patterns
- Develop caching and optimization strategies for improved access efficiency
- Integrate findings into the FORM I/O framework design

