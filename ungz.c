/* sample implementation of gzip decompressor
   a little modified version of source code
   described in article of Joshua Davies
   http://www.infinitepartitions.com/cgi-bin/showarticle.cgi?article=art023

   this version has workaround to support files > 32k */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

typedef struct {
  unsigned short id;
  unsigned char compression_method;
  unsigned char flags;
  unsigned char mtime[4];
  unsigned char extra_flags;
  unsigned char os;
} gzip_header; // RFC 1952

enum {
  FTEXT = 0x01,
  FHCRC = 0x02,
  FEXTRA = 0x04,
  FNAME = 0x08,
  FCOMMENT = 0x10
};

typedef struct {
  int end;
  int bit_length;
} huffman_range;

typedef struct {
  unsigned int len;
  unsigned int code;
} tree_node;

typedef struct huffman_node_t {
  int code; // -1 for non-leaf nodes
  struct huffman_node_t *zero;
  struct huffman_node_t *one;
} huffman_node;

unsigned char* src;
unsigned char* dest;
unsigned char mask = 1; // current bit position within buf; 8 is MSB

unsigned int read_bit() {
  unsigned int bit = (*src & mask) ? 1 : 0;
  mask <<= 1;
  if (!mask) mask = 0x01, src++;
  return bit;
}

int read_bits(int n) {
  int value = 0;
  while (n--) {
    value = (value << 1) | read_bit();
  }
  return value;
}

int read_bits_inv(int n) {
  int value = 0;
  for (int i = 0; i < n; i++) {
    value |= (read_bit() << i);
  }
  return value;
}

void build_huffman_tree(huffman_node *root, int range_len, huffman_range *range) {
  int num_codes = range[range_len-1].end + 1;

  // step 1 - figure out how long bl_count, next_code, tree etc.
  // should be based on the ranges provided;
  int max_bit_length = 0;
  for (int i = 0; i < range_len; i++) {
    if (range[i].bit_length > max_bit_length) {
      max_bit_length = range[i].bit_length;
    }
  }

  int* bl_count = malloc(sizeof(int) * (max_bit_length + 1));
  int* next_code = malloc(sizeof(int) * (max_bit_length + 1));
  tree_node* tree = malloc(sizeof(tree_node) * num_codes);
  memset(bl_count, 0, sizeof(int) * (max_bit_length + 1));
  memset(next_code, 0, sizeof(int) * (max_bit_length + 1));
  memset(tree, 0, sizeof(tree_node) * num_codes);

  for (int i = 0; i < range_len; i++) {
    bl_count[range[i].bit_length] +=
        range[i].end - ((i > 0) ? range[i - 1].end : -1);
  }

  // step 2, directly from RFC
  int code = 0;
  for (int bits = 1; bits <= max_bit_length; bits++) {
    code = (code + bl_count[bits - 1]) << 1;
    if (bl_count[bits]) {
      next_code[bits] = code;
    }
  }

  // step 3, directly from RFC
  int active_range = 0;
  for (int i = 0; i < num_codes; i++) {
    if (i > range[active_range].end) {
      active_range++;
    }
    if (range[active_range].bit_length) {
      tree[i].len = range[active_range].bit_length;
      if (tree[i].len != 0) {
        tree[i].code = next_code[tree[i].len];
        next_code[tree[i].len]++;
      }
    }
  }

  // Ok, now I have the codes... convert them into a traversable
  // huffman tree
  root->code = -1;
  for (int i = 0; i < num_codes; i++) {
    huffman_node *node;
    node = root;
    if (tree[i].len) {
      for (int bits = tree[i].len; bits; bits--) {
        if (tree[i].code & (1 << (bits - 1))) {
          if (!node->one) {
            node->one = (huffman_node*)malloc(sizeof(huffman_node));
            memset(node->one, 0, sizeof(huffman_node));
            node->one->code = -1;
          }
          node = (huffman_node *)node->one;
        } else {
          if (!node->zero) {
            node->zero = (huffman_node*)malloc(sizeof(huffman_node));
            memset(node->zero, 0, sizeof(huffman_node));
            node->zero->code = -1;
          }
          node = (huffman_node *)node->zero;
        }
      }
      assert(node->code == -1);
      node->code = i;
    }
  }

  free(bl_count);
  free(next_code);
  free(tree);
}

/*
 * Build a Huffman tree for the following values:
 *   0 - 143: 00110000  - 10111111     (8)
 * 144 - 255: 110010000 - 111111111    (9)
 * 256 - 279: 0000000   - 0010111      (7)
 * 280 - 287: 11000000  - 11000111     (8)
 * According to the RFC 1951 rules in section 3.2.2
 * This is used to (de)compress small inputs.
 */
void build_fixed_huffman_tree(huffman_node *root) {
  huffman_range range[4];

  range[0].end = 143;
  range[0].bit_length = 8;
  range[1].end = 255;
  range[1].bit_length = 9;
  range[2].end = 279;
  range[2].bit_length = 7;
  range[3].end = 287;
  range[3].bit_length = 8;

  build_huffman_tree(root, 4, range);
}

// build a huffman tree from input as specified in section 3.2.7
void read_huffman_tree(huffman_node *literals_root, huffman_node *distances_root) {
  int code_length_order[] = { /* order of the bit length code lengths */
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
  int i, j;
  int code_lengths[19];
  huffman_range code_length_ranges[19];
  huffman_range *range;
  huffman_node code_lengths_root;
  huffman_node *code_lengths_node;

  int hlit = read_bits_inv(5); // # of Literal/Length codes - 257 (257 - 286)
  int hdist = read_bits_inv(5); // # of Distance codes - 1        (1 - 32)
  int hclen = read_bits_inv(4); // # of Code Length codes - 4     (4 - 19)

  int nl = 257 + hlit;      /* number of literal/length codes */
  int nd = 1 + hdist;        /* number of distance codes */
  int nb = 4 + hclen;

  memset(code_lengths, 0, sizeof(code_lengths));
  for (i = 0; i < (hclen + 4); i++) {
    code_lengths[code_length_order[i]] = read_bits_inv(3);
  }

  // turn those into actionable ranges for the huffman tree routine
  j = 0; // j becomes the length of the range array
  for (i = 0; i < 19; i++) {
    if ((i > 0) && (code_lengths[i] != code_lengths[i - 1])) {
      j++;
    }
    code_length_ranges[j].end = i;
    code_length_ranges[j].bit_length = code_lengths[i];
  }

  memset(&code_lengths_root, 0, sizeof(huffman_node));
  build_huffman_tree(&code_lengths_root, j + 1, code_length_ranges);

  // read the literal/length lengths; this is encoded using the huffman
  // tree from the previous step
  i = 0;
  int* lengths = (int *)malloc((hlit + hdist + 258) * sizeof(int));
  range = (huffman_range *)malloc((hlit + hdist + 258) * sizeof(huffman_range));
  code_lengths_node = &code_lengths_root;
  while (i < (hlit + hdist + 258)) {
    if (read_bit()) {
      code_lengths_node = code_lengths_node->one;
    } else {
      code_lengths_node = code_lengths_node->zero;
    }

    if (code_lengths_node->code != -1) {
      if (code_lengths_node->code > 15) {
        int repeat_length;

        switch (code_lengths_node->code) {
        case 16:
          repeat_length = read_bits_inv(2) + 3;
          break;
        case 17:
          repeat_length = read_bits_inv(3) + 3;
          break;
        case 18:
          repeat_length = read_bits_inv(7) + 11;
          break;
        default:
          fprintf(stderr, "error in input stream\n");
          exit(1);
        }

        while (repeat_length--) {
          if (code_lengths_node->code == 16) {
            lengths[i] = lengths[i - 1];
          } else {
            lengths[i] = 0;
          }
          i++;
        }
      } else {
        lengths[i] = code_lengths_node->code;
        i++;
      }

      code_lengths_node = &code_lengths_root;
    }
  }

  // now, split the lengths in two parts and turn each into a huffman
  // tree

  // now the lengths lengths have been read.  turn _those_
  // into a valid range declaration and build the final huffman
  // code from it.

  j = 0;
  for (i = 0; i < nl; i++) {
    if ((i > 0) && (lengths[i] != lengths[i - 1])) {
      j++;
    }
    range[j].end = i;
    range[j].bit_length = lengths[i];
  }

  build_huffman_tree(literals_root, j + 1, range);

  j = 0;
  for (i = hlit + 257; i < (hdist + hlit + 258); i++) {
    if ((i > (257 + hlit)) && (lengths[i] != lengths[i - 1])) {
      j++;
    }
    range[j].end = i - (257 + hlit);
    range[j].bit_length = lengths[i];
  }

  build_huffman_tree(distances_root, j + 1, range);

  free(lengths);
  free(range);
}

int inflate(huffman_node* literals_root, huffman_node* distances_root) {
  const int extra_length_addend[] = {11, 13, 15, 17, 19, 23,  27,  31,  35,  43,
                                     51, 59, 67, 83, 99, 115, 131, 163, 195, 227};
  const int extra_dist_addend[] = {4,    6,    8,     12,    16,   24,   32,
                                   48,   64,   96,    128,   192,  256,  384,
                                   512,  768,  1024,  1536,  2048, 3072, 4096,
                                   6144, 8192, 12288, 16384, 24576};
  huffman_node* node;
  int stop_code = 0;
  node = literals_root;

  while (!stop_code) {
    if (read_bit()) {
      node = node->one;
    } else {
      node = node->zero;
    }
    if (node->code != -1) {
      // found a leaf in the tree; decode a symbol
      assert(node->code < 286); // should never happen (?)
      if (node->code < 256) {
        putchar(node->code);
        *(dest++) = node->code;
      } else if (node->code == 256) {
        stop_code = 1;
        break;
      } else if (node->code > 256) {
        int length;
        int dist;
        int extra_bits;
        // this is a back-pointer
        // interpret the length here as specified in 3.2.5
        if (node->code < 265) {
          length = node->code - 254;
        } else if (node->code < 285) {
          extra_bits = read_bits_inv((node->code - 261) / 4);
          length = extra_bits + extra_length_addend[node->code - 265];
        } else {
          length = 258;
        }
        // the length is followed by the distance.
        // the distance is coded in 5 bits, and may be
        // followed by extra bits as specified in 3.2.5
        if (distances_root == NULL) { // hardcoded distances
          dist = read_bits(5);
        } else { // dynamic distances
          node = distances_root;
          while (node->code == -1) {
            if (read_bit()) {
              node = node->one;
            } else {
              node = node->zero;
            }
          }
          dist = node->code;
        }
        if (dist > 3) {
          int extra_dist = read_bits_inv((dist - 2) / 2);
          // embed the logic in the table at the end of 3.2.5
          dist = extra_dist + extra_dist_addend[dist - 4];
        }

        unsigned char *backptr = dest - dist - 1;
        while (length--) {
          // Note that ptr & backptr can overlap
          putchar(*backptr);
          *(dest++) = *(backptr++);
        }
      }
      node = literals_root;
    }
  }
  return 0;
}

#if defined(__MSVCRT__) || defined(__OS2__) || defined(_MSC_VER)
#include <fcntl.h>
#include <io.h>
#endif

int main( int argc, const char **argv ) {

#if defined(__MSVCRT__) || defined(__OS2__) || defined(_MSC_VER)
    setmode( fileno( stdin ), O_BINARY );
    setmode( fileno( stdout ), O_BINARY );
#endif

  if (argc < 2) printf("usage: %s file \n", argv[0]), exit(1);
  FILE* f = fopen(argv[1], "rb");
  if (!f) return fprintf(stderr,"can not open file\n");
  fseek(f, 0, SEEK_END); // seek to end of file
  size_t n = ftell(f); // get current file pointer
  fseek(f, 0, SEEK_SET); // seek back to beginning of file
  src = malloc(n);
  if (!src) return fprintf(stderr,"can not allocate memory\n");
  fread(src, n, 1, f);
  gzip_header* hdr = (void*)src;
  uint16_t crc16;
  if (hdr->id != 0x8b1f) return fprintf(stderr,"bad signature\n");
  if (hdr->compression_method != 8) return fprintf(stderr,"unknown algorithm\n");
  src = src + sizeof(gzip_header);
  if (hdr->flags & FEXTRA) {
    int xlen = *(uint16_t*)src;
    //printf("file has extra information (%d bytes)\n", xlen);
    src += 2 + xlen;
  }
  if (hdr->flags & FNAME) {
    //printf("original name: %s\n", src);
    src += strlen(src)+1;
  }
  if (hdr->flags & FCOMMENT) {
    //printf("comment: %s\n", src);
    src += strlen(src)+1;
  }
  if (hdr->flags & FHCRC) {
    crc16 = *(uint16_t*)src;
    src += 2;
    //printf("crc16: %d", crc16);
  }
  if (hdr->flags & !(FNAME|FCOMMENT|FHCRC|FEXTRA)) {
    return fprintf(stderr, "unknwon flags\n");
  }
  // decompress
  dest = malloc(n*258);
  if (!dest) return fprintf(stderr, "can not allocate memory\n");
  huffman_node literals_root;
  huffman_node distances_root;
  unsigned last_block;
  do {
    last_block = read_bit();
    unsigned block_format = read_bits_inv(2);
    //printf("block format: %d \n", block_format);
    switch (block_format) {
      case 0x00:
        // note: this path is not tested
        if (mask != 0x01) mask = 0x01, src++;
        int len = *(uint16_t*)src;
        src += 2; // skip len
        src += 2; // skip nlen
        for (int i = 0; i < len; i++) {
          putchar(*src++);
        }
        break;
      case 0x01:
        memset(&literals_root, 0, sizeof(huffman_node));
        build_fixed_huffman_tree(&literals_root);
        inflate(&literals_root, NULL);
        break;
      case 0x02:
        memset(&literals_root, 0, sizeof(huffman_node));
        memset(&distances_root, 0, sizeof(huffman_node));
        read_huffman_tree(&literals_root, &distances_root);
        inflate(&literals_root, &distances_root);
        break;
      default:
        return fprintf(stderr, "unsupported block type %x\n", block_format );
    }
  } while (!last_block);
}
