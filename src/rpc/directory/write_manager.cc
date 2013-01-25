// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "rpc/directory/write_manager.tcc"

template class directory_write_manager_t<int>;

#include "clustering/administration/metadata.hpp"
template class directory_write_manager_t<cluster_directory_metadata_t>;

#include "clustering/reactor/directory_echo.hpp"
template class directory_write_manager_t<directory_echo_wrapper_t<std::string> >;
