#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include "memcache.h"

#define CHUNK_SIZE 1024
int CACHEFD;

struct dir_struct
{
    int index;
    int is_dir;
    mode_t mode;
    uid_t uid;
    gid_t gid;
    int data_length;
    int xattr_length;
    int name_length;
    char name[250];
};

struct data
{
    int parent_index;
    int index;
    int length;
    char data[CHUNK_SIZE];
};

int find_response_index(char *response, char ch)
{
    int i;
    for (i = 0; i < strlen(response); i++)
    {
        if (response[i] == ch)
        {
            // printf("vedzebt: %s - shi, pasuxi: %d\n", response, i);
            return i + 1;
        }
    }
    return -1;
}

/* 
Gets response from memcache for the index as a key, parses
and cast the response and returns the directory struct pointer.
 */
struct dir_struct *get_dir_by_index(int index)
{
    char *response = get(CACHEFD, index);
    int struct_index = find_response_index(response, '\n');
    if (struct_index == -1 || struct_index == 5)
    {
        free(response);
        return NULL;
    }
    struct dir_struct *result = malloc(sizeof(struct dir_struct));
    memcpy(result, response + struct_index, sizeof(struct dir_struct));
    free(response);
    return result;
}

/* Does the same thing as get_dir_by_index, only for data structure */
struct data *get_data_by_indexes(int parent_index, int index, int xattr)
{
    char *response = inode_get(CACHEFD, parent_index, index, xattr);
    int struct_index = find_response_index(response, '\n');
    if (struct_index < 12)
    {
        free(response);
        return NULL;
    }
    struct data *result = malloc(sizeof(struct data));
    memcpy(result, response + struct_index, sizeof(struct data));
    free(response);
    return result;
}

/*
Fills the buffer with the data of directory. In case of
success returns 0, -1 otherwise.
 */
int get_full_data(int parent_index, int data_length, char *buff, int xattr)
{
    if (parent_index == -1)
    {
        return -1;
    }
    int i;
    for (i = 0; i < data_length / CHUNK_SIZE; i++)
    {
        struct data *temp_data = get_data_by_indexes(parent_index, i, xattr);
        if (temp_data == NULL)
            return -1;
        memcpy(buff + i * CHUNK_SIZE, temp_data->data, CHUNK_SIZE);
        free(temp_data);
    }
    if (data_length % CHUNK_SIZE != 0)
    {
        struct data *temp_data = get_data_by_indexes(parent_index, data_length / CHUNK_SIZE, xattr);
        if (temp_data == NULL)
            return -1;
        memcpy(buff + data_length - (data_length % CHUNK_SIZE), temp_data->data, data_length % CHUNK_SIZE);
        free(temp_data);
    }
    return 0;
}

/* Returns directory for the given path */
struct dir_struct *get_dir_by_path(const char *path, int path_length)
{
    int index_h = index_hash(path, path_length);
    struct dir_struct *dir = get_dir_by_index(index_h);
    return dir;
}

/* Returns the index, where the parent path ends */
int get_parent_path_length(const char *path)
{
    int i;
    for (i = strlen(path) - 1; i >= 0; i--)
    {
        if (path[i] == '/')
            break;
    }
    if (i == -1)
    {
        // printf("AR YOFILA MTLIANI STRINGI\n");
        return -1;
    }
    if (i == 0)
    {
        i += 1;
    }
    return i;
}

/* Creates a new directory structure for the path and adds it to memcache */
int add_new_dir_struct(const char *path, int isdir, uid_t uid, gid_t gid, mode_t mode)
{
    int new_dir_index = index_hash(path, strlen(path));
    struct dir_struct new_dir;
    new_dir.data_length = 0;
    new_dir.index = new_dir_index;
    new_dir.is_dir = isdir;
    new_dir.uid = uid;
    new_dir.gid = gid;
    new_dir.mode = mode;
    new_dir.xattr_length = 0;
    new_dir.name_length = strlen(path);
    memcpy(new_dir.name, path, new_dir.name_length);
    big_command(CACHEFD, "set", new_dir_index, 0, 0, sizeof(struct dir_struct), (char *)&new_dir);
    return new_dir_index;
}

/* Creates new directory and adds it to the structure */
int add_dir(const char *path, int isdir, mode_t mode)
{
    int parent_end = get_parent_path_length(path);
    if (parent_end == -1)
    {
        return -1;
    }
    struct dir_struct *parent = get_dir_by_path(path, parent_end);
    if (parent == NULL)
    {
        return -1;
    }
    int new_dir_index = add_new_dir_struct(path, isdir, getuid(), getgid(), mode);
    if ((parent->data_length % CHUNK_SIZE) == 0)
    {
        struct data data;
        data.index = parent->data_length / CHUNK_SIZE;
        data.parent_index = parent->index;
        data.length = sizeof(int);
        memcpy(data.data, &new_dir_index, sizeof(int));
        inode_big_command(CACHEFD, "set", parent->index, data.index, sizeof(struct data), (char *)&data, 0);
    }
    else
    {
        struct data *dat = get_data_by_indexes(parent->index, parent->data_length / CHUNK_SIZE, 0);
        memcpy(dat->data + dat->length, &new_dir_index, sizeof(int));
        dat->length += sizeof(int);
        inode_big_command(CACHEFD, "set", parent->index, dat->index, sizeof(struct data), (char *)dat, 0);
        free(dat);
    }
    parent->data_length += sizeof(int);
    big_command(CACHEFD, "set", parent->index, 0, 0, sizeof(struct dir_struct), (char *)parent);
    free(parent);
    return 0;
}

int write_in_chunk_center(struct dir_struct *dir, const char *buf, size_t size, off_t offset)
{
    struct data *inode = get_data_by_indexes(dir->index, offset / CHUNK_SIZE, 0);
    if (inode == NULL)
    {
        inode = malloc(sizeof(struct data));
        inode->parent_index = dir->index;
        inode->index = offset / CHUNK_SIZE;
        inode->length = 0;
        bzero(inode->data, CHUNK_SIZE);
    }
    int num_write = CHUNK_SIZE - (offset % CHUNK_SIZE);
    if (num_write > size)
        num_write = size;
    memcpy(inode->data + offset % CHUNK_SIZE, buf, num_write);
    if (inode->length < (offset % CHUNK_SIZE) + num_write)
        inode->length = (offset % CHUNK_SIZE) + num_write;
    inode_big_command(CACHEFD, "set", dir->index, inode->index, sizeof(struct data), (char *)inode, 0);
    free(inode);
    return num_write;
}

int read_from_chunk_center(struct dir_struct *dir, char *buf, size_t size, off_t offset)
{
    if (offset % CHUNK_SIZE != 0)
    {
        struct data *inode = get_data_by_indexes(dir->index, offset / CHUNK_SIZE, 0);
        if (inode == NULL)
        {
            inode = malloc(sizeof(struct data));
            inode->length = 1024;
            bzero(inode->data, CHUNK_SIZE);
        }
        int num_read = CHUNK_SIZE - (offset % CHUNK_SIZE);
        if (num_read > size)
            num_read = size;
        memcpy(buf, inode->data + offset % CHUNK_SIZE, num_read);
        free(inode);
        return num_read;
    }
    return 0;
}

void delete_file_chunks(int parent_index, int new_length, int old_length)
{
    if (old_length <= new_length)
        return;
    int i;
    for (i = new_length / CHUNK_SIZE + 1; i < old_length / CHUNK_SIZE; i++)
    {
        inode_delete_command(CACHEFD, parent_index, i, 0);
    }
    if (new_length % CHUNK_SIZE == 0)
        inode_delete_command(CACHEFD, parent_index, new_length / CHUNK_SIZE, 0);
    if (old_length % CHUNK_SIZE != 0 && ((new_length / CHUNK_SIZE) != (old_length / CHUNK_SIZE)))
        inode_delete_command(CACHEFD, parent_index, old_length / CHUNK_SIZE, 0);
}

int get_permission(mode_t req, mode_t mode)
{
    mode_t result = req & mode;
    if (result == req)
        return 0;
    return -EACCES;
}

int get_access(int request, mode_t mode, uid_t uid, gid_t gid)
{
    int result = 0;
    if (uid == getuid())
        result = get_permission(request * 64, mode);
    if (gid == getgid())
        result = get_permission(request * 8, mode);
    if (uid != getuid() && gid != getgid())
        result = get_permission(request, mode);
    return result;
}

int check_path(const char *path)
{
    int i;
    for (i = 1; i < strlen(path); i++)
    {
        if (path[i] != '/')
            continue;
        struct dir_struct *dir = get_dir_by_path(path, i);
        if (dir == NULL)
            return -ENOENT;
        if (dir->is_dir == 0)
            return -ENOTDIR;
        if (get_access(4, dir->mode, dir->uid, dir->gid) != 0)
        {
            free(dir);
            return -EACCES;
        }
        free(dir);
    }
    return 0;
}

static void *fs_init(struct fuse_conn_info *conn,
                     struct fuse_config *cfg)
{
    (void)conn;
    cfg->kernel_cache = 1;
    CACHEFD = get_cachefd();
    command(CACHEFD, "flush_all");
    add_new_dir_struct("/", 1, getuid(), getgid(), (S_IFDIR | 0755));
    return NULL;
}

static void fs_destroy(void *private_data)
{
    printf("%s\n", "fs_destroy");
    (void *)private_data;
    close(CACHEFD);
}

static int fs_getattr(const char *path, struct stat *stbuf,
                      struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_getattr", path);
    (void)fi;
    memset(stbuf, 0, sizeof(struct stat));
    struct dir_struct *dir_file = get_dir_by_path(path, strlen(path));
    if (dir_file == NULL)
        return -ENOENT;
    stbuf->st_uid = dir_file->uid;
    stbuf->st_gid = dir_file->gid;
    if (dir_file->is_dir == 1)
    {
        stbuf->st_mode = S_IFDIR | dir_file->mode;
        stbuf->st_nlink = 2;
        stbuf->st_size = dir_file->name_length;
    }
    else
    {
        stbuf->st_mode = S_IFREG | dir_file->mode;
        stbuf->st_nlink = 1;
        stbuf->st_size = dir_file->data_length;
    }
    free(dir_file);
    return 0;
}

static int fs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    printf("%s for %s\n", "fs_readdir", path);
    (void)offset;
    (void)fi;
    (void)flags;
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
    {
        return -ENOENT;
    }
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    char data_buffer[dir->data_length];
    int check = get_full_data(dir->index, dir->data_length, data_buffer, 0);
    if (check != 0)
    {
        free(dir);
        return -ENOENT;
    }
    int i;
    for (i = 0; i < dir->data_length / sizeof(int); i++)
    {
        int *temp = (int *)data_buffer;
        temp += i;
        struct dir_struct *curr_dir = get_dir_by_index(*temp);
        if (curr_dir == NULL)
        {
            return -1;
        }
        char name_buff[curr_dir->name_length + 1];
        memcpy(name_buff, curr_dir->name, curr_dir->name_length);
        name_buff[curr_dir->name_length] = 0;
        int j;
        for (j = curr_dir->name_length; j > 0; j--)
        {
            if (name_buff[j] == '/')
                break;
        }
        filler(buf, name_buff + j + 1, NULL, 0, 0);
        free(curr_dir);
    }
    free(dir);
    return 0;
}

static int fs_mkdir(const char *path, mode_t mode)
{
    printf("%s for %s\n", "fs_mkdir", path);
    if (strlen(path) > 250)
        return -ENAMETOOLONG;
    int check = add_dir(path, 1, mode);
    if (check != 0)
    {
        return -1;
    }
    else
        return 0;
}

static int fs_rmdir(const char *path)
{
    printf("%s for %s\n", "fs_rmdir", path);
    int parent_end = get_parent_path_length(path);
    int parent_index = index_hash(path, parent_end);
    int index = index_hash(path, strlen(path));
    struct dir_struct *parent = get_dir_by_index(parent_index);
    struct dir_struct *dir = get_dir_by_index(index);
    if (dir->data_length != 0)
    {
        free(parent);
        free(dir);
        return -ENOTEMPTY;
    }
    char data_buff[parent->data_length];
    int check = get_full_data(parent_index, parent->data_length, data_buff, 0);
    if (check == -1)
    {
        return -1;
    }
    int i;
    for (i = 0; i < parent->data_length / sizeof(int); i++)
    {
        int *curr_index = (int *)data_buff;
        curr_index += i;
        if (index == *curr_index)
        {
            // Swap index with the last one
            memcpy(curr_index, data_buff + parent->data_length - sizeof(int), sizeof(int));
            // Update changed inode
            int inode_index = (i * sizeof(int)) / CHUNK_SIZE;
            struct data *inode = get_data_by_indexes(parent->index, inode_index, 0);
            memcpy(inode->data, data_buff + i * sizeof(int) - i * sizeof(int) % CHUNK_SIZE, inode->length);
            inode_big_command(CACHEFD, "set", parent_index, inode->index, sizeof(struct data), (char *)inode, 0);
            free(inode);
            // update last chunk by reducing data length
            inode_index = (parent->data_length - sizeof(int)) / CHUNK_SIZE;
            inode = get_data_by_indexes(parent->index, inode_index, 0);
            inode->length -= sizeof(int);
            if (inode->length != 0)
                inode_big_command(CACHEFD, "set", parent_index, inode->index, sizeof(struct data), (char *)inode, 0);
            free(inode);
            // update parent by reducing data length
            parent->data_length -= sizeof(int);
            big_command(CACHEFD, "set", parent_index, 0, 0, sizeof(struct dir_struct), (char *)parent);
            free(parent);
            delete_command(CACHEFD, index);
            free(dir);
            return 0;
        }
    }
    free(parent);
    free(dir);
    return -1;
}

static int fs_open(const char *path, struct fuse_file_info *fi)
{
    printf("OPENING %s FLAGS: %d\n", path, fi->flags);
    int readable_path = check_path(path);
    printf("readable path: %d\n", readable_path);
    if (readable_path != 0)
        return readable_path;
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    int result = 0;
    if (dir != NULL)
    {
        if (dir->uid == getuid())
            result = get_permission(fi->flags & S_IRWXU, dir->mode);
        if (dir->gid == getgid())
            result = get_permission(fi->flags & S_IRWXG, dir->mode);
        if (dir->uid != getuid() && dir->gid != getgid())
            result = get_permission(fi->flags & S_IRWXO, dir->mode);
        free(dir);
    }
    else
    {
        struct dir_struct *parent = get_dir_by_path(path, get_parent_path_length(path));
        if (parent == NULL)
            return -1;
        if (parent->uid == getuid())
            result = get_permission(S_IWUSR, parent->mode);
        if (parent->gid == getgid())
            result = get_permission(S_IWGRP, parent->mode);
        if (parent->uid != getuid() && parent->gid != getgid())
            result = get_permission(S_IWOTH, parent->mode);
        free(parent);
    }
    return result;
}

static int fs_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_write", path);
    char buffer[size + 1];
    memcpy(buffer, buf, size);
    buffer[size] = 0;
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
        return -1;
    if (size == 0)
        return 0;
    // At first, if offset starts in the middle of the chunk, write starting part
    int written = 0;
    if (offset % CHUNK_SIZE != 0)
    {
        written = write_in_chunk_center(dir, buf, size, offset);
        if (written == 0)
        {
            free(dir);
            return 0;
        }
    }
    // Check if that was enough
    if (written == size)
    {
        delete_file_chunks(dir->index, offset + size, dir->data_length);
        dir->data_length = offset + size;
        big_command(CACHEFD, "set", dir->index, 0, 0, sizeof(struct dir_struct), (char *)dir);
        free(dir);
        return written;
    }
    buf += written;
    size -= written;
    offset += written;
    // Write middle chunks
    int i;
    for (i = 0; i < size / CHUNK_SIZE; i++)
    {
        struct data inode;
        inode.parent_index = dir->index;
        inode.index = i + (offset / CHUNK_SIZE);
        memcpy(inode.data, buf + (i * CHUNK_SIZE), CHUNK_SIZE);
        inode_big_command(CACHEFD, "set", inode.parent_index, inode.index, sizeof(struct data), (char *)&inode, 0);
    }
    // Now write the last chunk
    if (size % CHUNK_SIZE != 0)
    {
        int chunk_index = (size + offset) / CHUNK_SIZE;
        // printf("CHUNK_INDEX:------------------------- %d\n", chunk_index);
        struct data *inode = get_data_by_indexes(dir->index, chunk_index, 0);
        if (inode == NULL)
        {
            inode = malloc(sizeof(struct data));
            inode->parent_index = dir->index;
            inode->index = chunk_index;
            inode->length = 0;
            bzero(inode->data, CHUNK_SIZE);
        }
        memcpy(inode->data, buf + size - (size % CHUNK_SIZE), size % CHUNK_SIZE);
        inode->length = size % CHUNK_SIZE;
        inode_big_command(CACHEFD, "set", dir->index, inode->index, sizeof(struct data), (char *)inode, 0);
        free(inode);
    }
    // Check if the
    if (dir->data_length > offset + size)
    {
        int delete_from = (offset + size) / CHUNK_SIZE;
    }
    delete_file_chunks(dir->index, offset + size, dir->data_length);
    dir->data_length = offset + size;
    big_command(CACHEFD, "set", dir->index, 0, 0, sizeof(struct dir_struct), (char *)dir);
    // }
    free(dir);
    return written + size;
}

static int fs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_create", path);
    if (strlen(path) > 250)
        return -ENAMETOOLONG;
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
    {
        int res = add_dir(path, 0, mode);
        if (res != 0)
            return res;
    }
    free(dir);
    return fs_open(path, fi);
}

static int fs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_utimens", path);
    return 0;
}

static int fs_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_read", path);
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
        return -1;
    if (size == 0 || dir->data_length == 0)
    {
        free(dir);
        return 0;
    }
    if (dir->data_length <= offset)
    {
        free(dir);
        return -1;
    }
    if (offset + size > dir->data_length)
        size = dir->data_length - offset;
    // At first, if offset starts in the middle of the chunk, write starting part
    int ready = read_from_chunk_center(dir, buf, size, offset);
    // Check if that was enough
    if (ready == -1)
    {
        free(dir);
        return 0;
    }
    if (ready == size)
    {
        free(dir);
        return ready;
    }
    buf += ready;
    size -= ready;
    offset += ready;
    // Write middle chunks
    int i;
    for (i = 0; i < size / CHUNK_SIZE; i++)
    {
        struct data *inode = get_data_by_indexes(dir->index, i + (offset / CHUNK_SIZE), 0);
        if (inode == NULL)
        {
            //
            // return ready + i * CHUNK_SIZE;
            inode = malloc(sizeof(struct data));
            bzero(inode->data, CHUNK_SIZE);
        }
        memcpy(buf + i * CHUNK_SIZE, inode->data, CHUNK_SIZE);
        free(inode);
    }
    // Now write the last chunk
    if (size % CHUNK_SIZE != 0)
    {
        struct data *inode = get_data_by_indexes(dir->index, (offset + size) / CHUNK_SIZE, 0);
        if (inode == NULL)
        {
            inode = malloc(sizeof(struct data));
            bzero(inode->data, CHUNK_SIZE);
        }
        memcpy(buf + size - (size % CHUNK_SIZE), inode->data, size % CHUNK_SIZE);
        free(inode);
    }
    free(dir);
    return ready + size;
}

int fs_fsync(const char *path, int num, struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_fsync", path);
    return 0;
}

int fs_fsyncdir(const char *path, int num, struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_fsyncdir", path);
    return 0;
}

int fs_flush(const char *path, struct fuse_file_info *fi)
{
    printf("%s for %s\n", "fs_flush", path);
    return 0;
}

static int fs_unlink(const char *path)
{
    printf("%s for %s\n", "fs_unlink", path);
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
        return 0;
    delete_file_chunks(dir->index, 0, dir->data_length);
    dir->data_length = 0;
    big_command(CACHEFD, "set", dir->index, 0, 0, sizeof(struct dir_struct), (char *)dir);
    fs_rmdir(path);
    free(dir);
    return 0;
}

static int fs_access(const char *path, int mode)
{
    int readable_path = check_path(path);
    if (readable_path != 0)
        return readable_path;
    printf("LOOKING FOR %s's access\n", path);
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
        return -1;
    if (getuid() == 0)
        mode = mode & 6; /////////
    int result = get_access(mode, dir->mode, dir->uid, dir->gid);
    free(dir);
    return result;
}

static int fs_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    // struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    // if (dir == NULL)
    //     return -1;
    // int attr_index = index_hash(name, strlen(name));
    // struct data *attr = get_data_by_indexes(dir->index, attr_index, 1);

    // int new_dir_index = add_new_dir_struct(path, isdir, getuid(), getgid(), mode);
    // if ((parent->data_length % CHUNK_SIZE) == 0)
    // {
    //     struct data data;
    //     data.index = parent->data_length / CHUNK_SIZE;
    //     data.parent_index = parent->index;
    //     data.length = sizeof(int);
    //     memcpy(data.data, &new_dir_index, sizeof(int));
    //     inode_big_command(CACHEFD, "set", parent->index, data.index, sizeof(struct data), (char *)&data, 0);
    // }
    // else
    // {
    //     struct data *dat = get_data_by_indexes(parent->index, parent->data_length / CHUNK_SIZE, 0);
    //     memcpy(dat->data + dat->length, &new_dir_index, sizeof(int));
    //     dat->length += sizeof(int);
    //     inode_big_command(CACHEFD, "set", parent->index, dat->index, sizeof(struct data), (char *)dat, 0);
    //     free(dat);
    // }
    // parent->data_length += sizeof(int);
    // big_command(CACHEFD, "set", parent->index, 0, 0, sizeof(struct dir_struct), (char *)parent);
    // free(parent);
    // return 0;

    return 0;
}

static int fs_getxattr(const char *path, const char *name, char *value, size_t size)
{
    return 0;
}

static int fs_listxattr(const char *path, char *list, size_t size)
{
    return 0;
}

static int fs_removexattr(const char *path, const char *list)
{
    return 0;
}

static int fs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    int readable_path = check_path(path);
    if (readable_path != 0)
        return -EACCES;
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
        return -ENOENT;
    if (dir->uid != geteuid())
    {
        if (getuid() < 0 || getuid() != geteuid())
        {
            free(dir);
            return -EPERM;
        }
    }
    dir->mode = mode;
    big_command(CACHEFD, "set", dir->index, 0, 0, sizeof(struct dir_struct), (char *)dir);
    free(dir);
    return 0;
}

static int fs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    int readable_path = check_path(path);
    if (readable_path != 0)
        return -EACCES;
    struct dir_struct *dir = get_dir_by_path(path, strlen(path));
    if (dir == NULL)
        return -ENOENT;
    if (dir->uid != geteuid())
    {
        if (getuid() < 0 || getuid() != geteuid())
        {
            free(dir);
            return -EPERM;
        }
    }
    if (uid != -1)
        dir->uid = uid;
    if (gid != -1)
        dir->gid = gid;
    big_command(CACHEFD, "set", dir->index, 0, 0, sizeof(struct dir_struct), (char *)dir);
    free(dir);
    return 0;
}

static struct fuse_operations fs_oper = {
    .init = fs_init,
    .destroy = fs_destroy,
    .getattr = fs_getattr,
    .readdir = fs_readdir,
    .mkdir = fs_mkdir,
    .rmdir = fs_rmdir,
    .open = fs_open,
    .read = fs_read,
    .write = fs_write,
    .create = fs_create,
    .utimens = fs_utimens,
    .fsync = fs_fsync,
    .fsync = fs_fsyncdir,
    .flush = fs_flush,
    .unlink = fs_unlink,
    .access = fs_access,
    .setxattr = fs_setxattr,
    .getxattr = fs_getxattr,
    .listxattr = fs_listxattr,
    .removexattr = fs_removexattr,
    .chmod = fs_chmod,
    .chown = fs_chown,
};

int main(int argc, char *argv[])
{
    int ret;
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    ret = fuse_main(args.argc, args.argv, &fs_oper, NULL);
    return ret;
}