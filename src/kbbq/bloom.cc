#include <stdlib.h>
#include <cstring>
#include <cmath>
#include <random>
#include "bloom.hh"

namespace bloom
{
	std::vector<uint64_t> hash_seq(std::string seq, int k){
		int i, l;
		uint64_t x[2];
		uint64_t mask = (1ULL<<k*2) - 1;
		uint64_t shift = (k-1) * 2;
		std::vector<uint64_t> ret;
		ret.reserve(seq.length() - k + 1);
		for (i = l = 0, x[0] = x[1] = 0; i < seq.length(); ++i) {
			int c = seq_nt4_table[seq[i]];
			if (c < 4) { // not an "N" base
				x[0] = (x[0] << 2 | c) & mask;                  // forward strand
				x[1] = x[1] >> 2 | (uint64_t)(3 - c) << shift;  // reverse strand
				if (++l >= k) { // we find a k-mer
					uint64_t y = x[0] < x[1]? x[0] : x[1];
					ret.push_back(yak_hash64(y, mask));
				}
			} else l = 0, x[0] = x[1] = 0; // if there is an "N", restart
		}
		return ret;
	}

	void subsample_and_insert(bloomary_t& bfs, std::vector<uint64_t> hashes, double alpha, minion::Random& rng){
		int mask = (1<<PREFIXBITS)-1;
		std::bernoulli_distribution dist(alpha);
		for(uint64_t h: hashes){
			if(dist(rng)){
				bfs[h&mask].insert(h >> PREFIXBITS);
			}
		}
	}

	std::array<std::vector<int>,2> overlapping_kmers_in_bf(std::string seq, bloomary_t& b, int k){
		int i, l; //i is the character index, l is the length of the current stretch of non-N bases
		uint64_t x[2];
		uint64_t mask = (1ULL<<k*2) - 1;
		uint64_t shift = (k-1) * 2;
		std::vector<bool> kmer_possible(seq.length(), false); //the kmer ending in this position exists
		std::vector<bool> kmer_present(seq.length(), false); //the kmer ending in this position is present
		//possible is the sum of the relevant kmer_possibles
		//inbf is the sum of the relevant kmer_presents
		std::vector<int> inbf(seq.length(),0); //inbf and possible indices are in i space.
		std::vector<int> possible(seq.length(),0);
		int n_in = 0;
		int n_out = 0;
		for (i = l = 0, x[0] = x[1] = 0; i < seq.length(); ++i) {
			int c = seq_nt4_table[seq[i]];
			if (c < 4) { // not an "N" base
				x[0] = (x[0] << 2 | c) & mask;                  // forward strand
				x[1] = x[1] >> 2 | (uint64_t)(3 - c) << shift;  // reverse strand
				if (++l >= k) { // we find a k-mer
					// n_possible = n_possible + 1 <= k ? n_possible + 1 : k;
					kmer_possible[i] = true;
					uint64_t y = x[0] < x[1]? x[0] : x[1];
					uint64_t h = yak_hash64(y, mask);
					bool k_in = b[h&((1<<PREFIXBITS)-1)].query(h>>PREFIXBITS);
					kmer_present[i] = k_in;
					if(k_in){
						n_in++; //n_in += !!k_in; n_out += !!!k_in
					} else{
						n_out++;
					}
				} else {
					kmer_possible[i] = false;
				}
			} else{  // if there is an "N", restart
				l = 0, x[0] = x[1] = 0;
				kmer_possible[i] = false;
			}

			//we have a long enough stretch of kmers that the old ones start to matter
			if(i-k >= 0 && kmer_possible[i-k]){ //1 before the base we are tallying
				if(kmer_present[i-k]){
					n_in--;
				} else {
					n_out--;
				}
			}

			if(i - k + 1 >= 0){
				inbf[i -k + 1] = n_in;
				possible[i -k + 1] = n_in + n_out; //n_possible = n_in + n_out
			}
		}
		if(l >= k){ //we ended with a full kmer so we have to deal with the end positions
			for(; i < seq.length()+k-1; ++i){
				if(i-k >= 0 && kmer_possible[i-k]){
					if(kmer_present[i-k]){
						n_in--;
					}
					else {
						n_out--;
					}
				}
				if(i-k+1 >= 0){
					inbf[i - k + 1] = n_in;
					possible[i - k + 1] = n_in + n_out;
				}
			}
		}
		std::array<std::vector<int>,2> ret = {inbf, possible};
		return ret;
	}

	int nkmers_in_bf(std::string seq, bloomary_t& b, int k){
		std::vector<uint64_t> hashes = hash_seq(seq, k);
		int c = 0;
		for(uint64_t h: hashes){
			if(b[h&((1<<PREFIXBITS)-1)].query(h>>PREFIXBITS)){
				c++;
			}
		}
		return c;
	}

	std::array<size_t, 2> find_longest_trusted_seq(std::string seq, bloomary_t& b, int k){
		std::vector<uint64_t> hashes = bloom::hash_seq(seq, k);
		size_t i, l, n;
		size_t anchor_start, anchor_end, anchor_l, anchor_current;
		anchor_start = anchor_end = -1;
		anchor_l = anchor_current = 0;
		for(i = l = n = 0; i < seq.length(); ++i){
			int c = seq_nt4_table[seq[i]];
			if(c < 4){
				if(++l > k){
					uint64_t h = hashes[n++];
					if(b[h&((1<<PREFIXBITS)-1)].query(h>>PREFIXBITS)){
						anchor_current++; //length of current stretch
					} else {
						if(anchor_current > anchor_l){
							anchor_l = anchor_current;
							anchor_end = i;
							anchor_start = i + 1 - k - anchor_l;
						}
						anchor_current = 0;
					}
				}
			} else {
				if(anchor_current > anchor_l){
					anchor_l = anchor_current;
					anchor_end = i-1;
					anchor_start = i - k - anchor_l;
				}
				anchor_current = 0;
				l = 0;
			}
		}
		if(anchor_current > anchor_l){
			anchor_l = anchor_current;
			anchor_end = std::string::npos;
			anchor_start = i - k - anchor_l;
		}
		return std::array<size_t,2>{{anchor_start, anchor_end}};
	}

	void increment_coded_kmer(uint64_t x[], int c, int k){
		uint64_t mask = (1ULL<<k*2) - 1;
		uint64_t shift = (k-1) * 2;
		if(c < 4){
			x[0] = (x[0] << 2 | c) & mask;                  // forward strand
			x[1] = x[1] >> 2 | (uint64_t)(3 - c) << shift;  // reverse strand
		}
	}

	std::pair<std::vector<char>, int> find_longest_fix(std::string seq, bloomary_t& t, int k){
		//this function is probably not very efficient.
		size_t i;
		int l;
		int best_l = 0;
		uint64_t x[2];
		uint64_t mask = (1ULL<<k*2) - 1;
		std::vector<char> best_c;
		char original_c;
		size_t end = std::min(seq.length(), 2UL * k - 1);
		original_c = seq[k-1];
		for(const char* e : {"A","C","G","T"} ){
			char d = *e;
			if( d != original_c ){
				seq[k-1] = d;
				// current_l = nkmers_in_bf(seq.substr(0, end), t, k); //this is not very efficient but that's ok
				for(i = l = 0, x[0] = x[1] = 0; i < end; ++i){
					int c = seq_nt4_table[seq[i]];
					if (c < 4) { // not an "N" base
						increment_coded_kmer(x, c, k);
						if (++l >= k) { // we find a k-mer
							uint64_t y = x[0] < x[1]? x[0] : x[1];
							uint64_t h = yak_hash64(y, mask);
							if(!t[h&((1<<PREFIXBITS)-1)].query(h>>PREFIXBITS)){
								break; //if it's not trusted, end here
							}
						}
					} else {
						break;
					}
				}
				if(l > best_l){
					best_l = l;
					best_c.clear();
					best_c.push_back(d);
				} else if (l == best_l){
					best_c.push_back(d);
				}
			}
		}
		if(best_l == seq.length() && best_c.size() > 1){ //we ran out of kmers to try and we have a tie
			for(const char* e : {"A","C","G","T"} ){
				char d = *e;
				if( d != original_c ){
					seq = seq.substr(0, end); //original sequence length
					seq[k-1] = d;
					for(i = end - k, l = 0, x[0] = x[1] = 0; i < 2 * k - 1; ++i){
						int cur_seqlen = seq.length();
						for(const char* ex : {"A","C","G","T"}){	
							char extra = *ex;	
							int c = i < seq.length() ? seq_nt4_table[seq[i]] : seq_nt4_table[extra];
							if (c < 4) { // not an "N" base
								increment_coded_kmer(x, c, k);
								if (++l >= k) { // we find a k-mer
									uint64_t y = x[0] < x[1]? x[0] : x[1];
									uint64_t h = yak_hash64(y, mask);
									if(i >= seq.length() && t[h&((1<<PREFIXBITS)-1)].query(h>>PREFIXBITS)){
										seq.push_back(extra);
										break; //if we have an extra and it's trusted, end here
									}
								}
							}
						}
						if(cur_seqlen == seq.length()){ //we didn't add an extra base
							break; //exit extension
						}
					}
					if(l > best_l){
						best_l = l;
						best_c.clear();
						best_c.push_back(d);
					} else if (l == best_l){
						best_c.push_back(d);
					}
				}
			}
		}
		return std::make_pair(best_c, k - best_l);
	}

	Bloom::Bloom(int nshift, int nhashes): nshift(nshift), nhashes(nhashes), ninserts(0),
		//-3 is divide by 8, so we have nshift bits
		bloom(static_cast<uint8_t*>(aligned_alloc(1<<(YAK_BLK_SHIFT-3), 1ULL<<(nshift-3))), 
			[](uint8_t* p){free(p);}) {
		//nshift + YAK_BLK_SHIFT should be less than 64 (nshift <= 55)
		//nshift should be greater than or equal to YAK_BLK_SHIFT (9)
		//thus 9 <= nshift <= 55
		//	this->bloom = std::move( std::unique_ptr<uint8_t[]>(
		// 	static_cast<uint8_t*>(aligned_alloc(1<<(YAK_BLK_SHIFT-3), 1ULL<<(nshift-3)))
		// 	));
		bzero(this->bloom.get(), 1ULL<<(nshift-3));
	}

	Bloom::~Bloom(){
		// delete this->bloom;
	}

	int Bloom::insert(unsigned long long hash){
		int x = this->nshift - YAK_BLK_SHIFT; // the bloom filter size
		unsigned long long y = hash & ((1ULL<<x)-1); //last x bits of hash;
		int h1 = hash >> x & YAK_BLK_MASK; // 8 bits right before y;
		int h2 = hash >> this->nshift & YAK_BLK_MASK; //8 bits right before h1;
		uint8_t *p = &this->bloom[y << (YAK_BLK_SHIFT-3)];
		if((h2&31) == 0){
			h2 = (h2 + 1) & YAK_BLK_MASK;
		}
		int z = h1;
		int count = 0;
		for(int i = 0; i < this->nhashes; z = (z + h2) & YAK_BLK_MASK){
			uint8_t *q = &p[z>>3];
			uint8_t u = 1 << (z&7);
			count += !!(*q & u); // !! = double negate; makes 1 or 0
			*q |= u;
			++i;
		}
		++ninserts;
		return count;
	}

	int Bloom::query_n(unsigned long long hash){
		int x = this->nshift - YAK_BLK_SHIFT; // the bloom filter size
		unsigned long long y = hash & ((1ULL<<x)-1); // fit the hash into the bloom filter;
		//discard the beginning (which determines which filter the hash goes into to begin with)
		int h1 = hash >> x & YAK_BLK_MASK; // this is the beginning part that's not in y;
		int h2 = hash >> this->nshift & YAK_BLK_MASK; //this is some middle part of the hash;
		uint8_t *p = &this->bloom[y << (YAK_BLK_SHIFT-3)];
		if((h2&31) == 0){
			h2 = (h2 + 1) & YAK_BLK_MASK;
		}
		int z = h1;
		int count = 0;
		for(int i = 0; i < this->nhashes; z = (z + h2) & YAK_BLK_MASK){
			uint8_t *q = &p[z>>3];
			uint8_t u = 1 << (z&7);
			count += !!(*q & u); // !! = double negate; makes 1 or 0
			++i;
		}
		return count;
	}

	inline bool Bloom::query(unsigned long long hash){
		return (this->query_n(hash) == this->nhashes);
	}

	//fprate given approximate number of times bf was loaded
	double Bloom::fprate(unsigned long long n){
		int m = 1ULL<<(this->nshift); //number of bits in the filter
		int k = this -> nhashes;
		return pow(1 - exp(-1.0 * k * n / m), k);
	}

	//there are 2 ** shift bits in the filter.
	int Bloom::optimal_nhashes(int shift, int n){
		return floor(1.0 * (std::pow(2,shift)) / n * log(2));
	}

	// bool Bloom::kmers_in(std::string seq, int k){
	// 	std::vector<uint64_t> hashes = hash_seq(seq, k);
	// 	std::vector<bool> in(hashes.size());
	// 	for(int i = 0; i < hashes.size(); i++){
	// 		in[i] = this->query(hashes[i]);
	// 	}
	// }

	/* //let's consider the case when nshift is minimal; or nshift = YAK_BLK_SHIFT (9)
	Bloom::insert(unsigned long long hash){
		int h1 = hash & YAK_BLK_MASK; //last 8 bits
		int h2 = hash >> 9 & YAK_BLK_MASK; //skip one then get the second to last 8 bits
		// this looks like:
		//... [h2] [h2] [h2] [h2] [h2] [h2] [h2] [h2] [] [h1] [h1] [h1] [h1] [h1] [h1] [h1] [h1]
		//                                                 z    z    z    z    z <- initial address in bf
		//                                                            chosen bit -> z    z    z     
		uint8_t *p = &this->bloom[0];
		if((h2&31) == 0){ //if last 5 bits of h2 == 0;
			h2 = (h2 + 1) & YAK_BLK_MASK; //now last 5 bits == 1
		}
		int z = h1; // z = last 8 bits of hash
		int count = 0; //initialize count
		for(int i = 0; i < this->nhashes; z = (z + h2) & YAK_BLK_MASK){ //for every hash fn;
			uint8_t *q = &p[z >> 3]; //q = pointer to some part of the bloom filter
			uint8_t u = 1 << (z&7); // 1 << (last 3 bits of z); the bit chosen to look at
			count += !!(*q & u); // increment count if it's there
			q |= u; //update filter
			++i; // go to next hash
			//now z = z + h2
		}


	} */

	long double calculate_fpr(bloomary_t& bf){
		uint64_t m = 0; // bf[0].nshift; //number of bits
		int k = 0; //bf[0].nhashes; //number of hashes; we take an average and round i thnk
		uint64_t n = 0; //number of insertions
		for(Bloom b : bf){
			m += b.nshift;
			k += b.nhashes;
			n += b.ninserts;
		}
		k /= bf.size();
		return exp((long double)k * log1p(-exp(-(long double)k * n / m))); // (1 - exp(-kn/m))^k
	}

	long double calculate_phit(bloomary_t& bf, long double alpha){
		long double fpr = calculate_fpr(bf);
		double exponent = alpha < 0.1 ? 0.2 / alpha : 2;
		long double pa = 1 - pow(1-alpha,exponent);
		return pa + fpr - fpr * pa;
	}

	uint64_t numbits(uint64_t numinserts, long double fpr){
		//m = - n * log2(fpr) / ln2
		return - numinserts * log2(fpr) / log(2);
	}

	int numhashes(long double fpr){
		//k = -log2(fpr)
		return -log2(fpr);
	}

}






