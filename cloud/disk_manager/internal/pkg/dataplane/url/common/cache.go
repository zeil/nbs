package common

import (
	"sort"
	"sync"
)

////////////////////////////////////////////////////////////////////////////////

// 4 MiB.
const chunkSize = 4 * 1024 * 1024

// 1 GiB.
const maxCacheSize = 1024 * 1024 * 1024

////////////////////////////////////////////////////////////////////////////////

type chunk struct {
	start uint64
	data  []byte
}

func (c *chunk) read(start uint64, data []byte) uint64 {
	startOffset := start - c.start
	endOffset := startOffset + uint64(len(data))
	if endOffset > uint64(len(c.data)) {
		endOffset = uint64(len(c.data))
	}

	copy(data, c.data[startOffset:endOffset])
	return endOffset - startOffset
}

////////////////////////////////////////////////////////////////////////////////

type cache struct {
	chunkPool   sync.Pool
	chunks      map[uint64]*chunk
	chunksMutex sync.Mutex
}

func newCache() *cache {
	return &cache{
		chunkPool: sync.Pool{
			New: func() interface{} {
				return &chunk{
					data: make([]byte, chunkSize),
				}
			},
		},
		chunks: make(map[uint64]*chunk),
	}
}

////////////////////////////////////////////////////////////////////////////////

func (c *cache) readChunk(
	start,
	chunkStart uint64,
	data []byte,
) (uint64, bool) {

	c.chunksMutex.Lock()
	defer c.chunksMutex.Unlock()

	retrievedChunk, ok := c.chunks[chunkStart]
	if !ok {
		return 0, false
	}
	return retrievedChunk.read(start, data), true
}

func (c *cache) read(
	start uint64,
	data []byte,
	readOnCacheMiss func(start uint64, data []byte) error,
) error {

	end := start + uint64(len(data))
	for start < end {
		chunkStart := (start / chunkSize) * chunkSize
		bytesRead, ok := c.readChunk(start, chunkStart, data)

		if !ok {
			// We read at most one chunk,
			// so we will retrieve at most 2 chunks and save them
			// to cache (in case the read data crosses the border between two
			// chunks).
			retrievedChunk := c.chunkPool.Get().(*chunk)
			retrievedChunk.start = chunkStart
			err := readOnCacheMiss(retrievedChunk.start, retrievedChunk.data)
			if err != nil {
				return err
			}

			c.put(retrievedChunk)
			bytesRead = retrievedChunk.read(start, data)
		}

		data = data[bytesRead:]
		start = chunkStart + chunkSize
	}

	return nil
}

// Not thread-safe.
func (c *cache) size() int {
	return len(c.chunks) * chunkSize
}

func (c *cache) put(chunk *chunk) {
	c.chunksMutex.Lock()
	defer c.chunksMutex.Unlock()

	if c.size() >= maxCacheSize {
		var keys []uint64
		for key := range c.chunks {
			keys = append(keys, key)
		}
		sort.Slice(keys, func(i, j int) bool { return keys[i] < keys[j] })

		for _, key := range keys {
			c.chunkPool.Put(c.chunks[key])
			delete(c.chunks, key)

			if c.size() < maxCacheSize {
				break
			}
		}
	}

	c.chunks[chunk.start] = chunk
}