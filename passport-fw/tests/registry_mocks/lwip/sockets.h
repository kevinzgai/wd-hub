#pragma once
inline int mock_socket_cleanups = 0;
inline void lwip_socket_thread_cleanup() { ++mock_socket_cleanups; }
