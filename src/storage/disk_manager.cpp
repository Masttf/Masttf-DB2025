/* Copyright (c) 2023 Renmin University of China
RMDB is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL
v2. You may obtain a copy of Mulan PSL v2 at:
        http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <string.h>    // for memset
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek
#include <dirent.h>    // for DIR, opendir, readdir, closedir
#include <errno.h>     // for errno
#include <fcntl.h>     // for open flags

#include "defs.h"

DiskManager::DiskManager() {
    // 初始化fd2pageno_数组
    for (int i = 0; i < MAX_FD; i++) {
        fd2pageno_[i].store(0);
    }
}

/**
 * @description: 将数据写入文件的指定磁盘页面中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 写入目标页面的page_id
 * @param {char} *offset 要写入磁盘的数据
 * @param {int} num_bytes 要写入磁盘的数据大小
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char *offset, int num_bytes) {
    if (fd < 0) {
        throw InternalError("Invalid file descriptor in write_page");
    }

    off_t write_offset = static_cast<off_t>(page_no) * PAGE_SIZE;

    ssize_t bytes_written = pwrite(fd, offset, num_bytes, write_offset);
    
    if (bytes_written != num_bytes) {
        if (errno == ENOSPC || errno == EDQUOT) {
             throw InternalError("Failed to write page due to no space");
        }
        throw InternalError("Failed to write page");
    }
}

/**
 * @description: 读取文件中指定编号的页面中的部分数据到内存中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 指定的页面编号
 * @param {char} *offset 读取的内容写入到offset中
 * @param {int} num_bytes 读取的数据量大小
 */
void DiskManager::read_page(int fd, page_id_t page_no, char *offset, int num_bytes) {
    if (fd < 0) {
        throw InternalError("Invalid file descriptor in read_page");
    }

    off_t offset_in_file = static_cast<off_t>(page_no) * PAGE_SIZE;
    
    // 使用pread避免竞争条件，无需使用lseek
    ssize_t bytes_read = pread(fd, offset, num_bytes, offset_in_file);

    if (bytes_read == 0) {
        // 如果读取到文件末尾，返回一个全0的页面
        memset(offset, 0, num_bytes);
    } else if (bytes_read != num_bytes) {
        throw InternalError("Failed to read page");
    }
}

/**
 * @description: 分配一个新的页号
 * @return {page_id_t} 分配的新页号
 * @param {int} fd 指定文件的文件句柄
 */
page_id_t DiskManager::allocate_page(int fd) {
    if (fd < 0 || fd >= MAX_FD) {
        throw InternalError("Invalid file descriptor in allocate_page");
    }
    // 文件已在创建时预分配，这里只增加逻辑页面计数
    page_id_t page_no = fd2pageno_[fd].fetch_add(1, std::memory_order_relaxed);

    return page_no;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {
    // 由于文件系统的限制，我们不实际收缩文件
    // 只在重新分配时覆盖这些页面
}

bool DiskManager::is_dir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string &path) {
    // 递归创建父目录
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!parent.empty() && !is_dir(parent)) {
            if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST) {
                throw UnixError();
            }
        }
    }
    
    // 创建目标目录
    if (!path.empty() && !is_dir(path)) {
        if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST) {
            throw UnixError();
        }
    }
}

// 文件删除回调函数
static int remove_callback(const char *path, const struct stat *sb,
                           int typeflag, struct FTW *ftwbuf) {
    return remove(path);
}

void DiskManager::destroy_dir(const std::string &path) {
    if (!is_dir(path)) {
        throw UnixError();
    }

    // 获取目录下的所有文件和子目录
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        throw UnixError();
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = path + "/" + entry->d_name;
        struct stat st;
        if (lstat(full_path.c_str(), &st) == -1) {
            closedir(dir);
            throw UnixError();
        }

        if (S_ISDIR(st.st_mode)) {
            destroy_dir(full_path);  // 递归删除子目录
        } else {
            if (unlink(full_path.c_str()) == -1) {
                closedir(dir);
                throw UnixError();
            }
        }
    }
    
    closedir(dir);
    
    // 删除空目录
    if (rmdir(path.c_str()) == -1) {
        throw UnixError();
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string &path) {
    // 用struct stat获取文件信息
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string &path) {
    // 检查文件是否已存在
    if (is_file(path)) {
        throw FileExistsError(path);
    }

    // 检查并创建父目录
    size_t last_slash = path.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir_path = path.substr(0, last_slash);
        if (!dir_path.empty() && !is_dir(dir_path)) {
            create_dir(dir_path);
        }
    }

    // 创建文件
    int fd = open(path.c_str(), O_CREAT | O_RDWR, 0644);
    if (fd < 0) {
        throw UnixError();
    }

    // 预分配文件大小
    off_t preallocate_size = static_cast<off_t>(4) * PAGE_SIZE;
    if (ftruncate(fd, preallocate_size) < 0) {
        close(fd);
        unlink(path.c_str()); // 创建失败，删除文件
        throw UnixError();
    }

    if (close(fd) < 0) {
        // 即使关闭失败，文件也可能已创建并截断
        throw UnixError();
    }
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string &path) {
    // 调用unlink()函数
    // 注意不能删除未关闭的文件

    // 检查文件是否存在
    if (!is_file(path)) {
        // 文件不存在，直接返回
        throw FileNotFoundError(path);
    }

    // 检查文件是否已打开
    if (path2fd_.find(path) != path2fd_.end()) {
        throw FileNotClosedError(path);
    }

    // 删除文件
    if (unlink(path.c_str()) < 0) {
        throw UnixError();
    }
}

/**
 * @description: 打开指定路径文件
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string &path) {
    // 检查是否已经打开
    auto it = path2fd_.find(path);
    if (it != path2fd_.end()) {
        return it->second;
    }

    // 检查文件是否存在
    if (!is_file(path)) {
        throw FileNotFoundError(path);
    }

    // 打开文件
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throw UnixError();
    }

    // 将文件描述符加入到映射中
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    
    // 初始化页面计数为0
    fd2pageno_[fd].store(0);

    return fd;
}

/**
 * @description:用于关闭指定路径文件
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotOpenError(fd);
    }

    // 文件大小在创建时已固定，关闭时不需要再调整
    if (close(fd) < 0) {
        throw UnixError();
    }
    path2fd_.erase(it->second);
    fd2path_.erase(it);
    fd2pageno_[fd].store(0, std::memory_order_relaxed); // 重置计数器
}

/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_size(const std::string &file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    // if (!fd2path_.count(fd)) {
    //     throw FileNotOpenError(fd);
    // }
    // return fd2path_[fd];

    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotOpenError(fd);
    }
    return it->second;
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string &file_name) {
    // if (!path2fd_.count(file_name)) {
    //     return open_file(file_name);
    // }
    // return path2fd_[file_name];

    auto it = path2fd_.find(file_name);
    if (it == path2fd_.end()) {
        return open_file(file_name);
    }
    return it->second;
}

/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
int DiskManager::read_log(char *log_data, int size, int offset) {
    // read log file from the previous end
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if (size == 0) return 0;
    lseek(log_fd_, offset, SEEK_SET);
    ssize_t bytes_read = read(log_fd_, log_data, size);
    assert(bytes_read == size);
    return bytes_read;
}

/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(char *log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }

    // 获取当前文件大小
    struct stat st;
    if (fstat(log_fd_, &st) < 0) {
        throw UnixError();
    }

    // 从文件末尾写入，使用pwrite避免竞争
    ssize_t bytes_write = pwrite(log_fd_, log_data, size, st.st_size);
    if (bytes_write != size) {
        throw UnixError();
    }
}