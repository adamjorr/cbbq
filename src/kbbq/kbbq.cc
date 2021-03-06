#include "htsiter.hh"
#include "bloom.hh"
#include "covariateutils.hh"
#include "recalibrateutils.hh"
#include <memory>
#include <iostream>
#include <fstream>
#include <sstream>
#include <htslib/hfile.h>
#include <htslib/thread_pool.h>
#include <getopt.h>
#include <cassert>
#include <functional>
#include <iomanip>
#include <ctime>

#ifndef NDEBUG
#define KBBQ_USE_RAND_SAMPLER
#endif

//opens file filename and returns a unique_ptr to the result.
std::unique_ptr<htsiter::HTSFile> open_file(std::string filename, htsThreadPool* tp, bool is_bam = true, bool use_oq = false, bool set_oq = false){
	std::unique_ptr<htsiter::HTSFile> f(nullptr);
	if(is_bam){
		f = std::move(std::unique_ptr<htsiter::BamFile>(new htsiter::BamFile(filename, tp, use_oq, set_oq)));
		// f.reset(new htsiter::BamFile(filename));
	} else {
		f = std::move(std::unique_ptr<htsiter::FastqFile>(new htsiter::FastqFile(filename, tp)));
		// f.reset(new htsiter::FastqFile(filename));
	}
	return f;
}



template<typename T>
std::ostream& operator<< (std::ostream& stream, const std::vector<T>& v){
	stream << "[";
	std::copy(v.begin(), v.end(), std::ostream_iterator<T>(stream, ", "));
	stream << "]";
	return stream;
}

template<typename T>
void print_vec(const std::vector<T>& v){
	std::cerr << v;
}

std::ostream& put_now(std::ostream& os){
	std::time_t t = std::time(nullptr);
	std::tm tm = *std::localtime(&t);
	return os << std::put_time(&tm, "[%F %T %Z]");
}

int check_args(int argc, char* argv[]){
	if(argc < 2){
		std::cerr << put_now << " Usage: " << argv[0] << " input.[bam,fq]" << std::endl;
		return 1;
	} else {
		std::cerr << put_now << "  Selected file: " << std::string(argv[1]) << std::endl;
		return 0;
	}
}

//long option, required arg?, flag, value
struct option long_options[] = {
	{"ksize",required_argument,0,'k'}, //default: 31
	{"use-oq",no_argument,0,'u'}, //default: off
	{"set-oq",no_argument,0,'s'}, //default: off
	{"genomelen",required_argument,0,'g'}, //estimated for bam input, required for fastq input
	{"coverage",required_argument,0,'c'}, //default: estimated
	{"fixed",required_argument,0,'f'}, //default: none
	{"alpha",required_argument,0,'a'}, //default: 7 / coverage
	{"threads",required_argument,0,'t'},
#ifndef NDEBUG
	{"debug",required_argument,0,'d'},
#endif
	{0, 0, 0, 0}
};

int main(int argc, char* argv[]){
	int k = 32;
	long double alpha = 0;
	uint64_t genomelen = 0; //est w/ index with bam, w/ fq estimate w/ coverage
	uint coverage = 0; //if not given, will be estimated.
	uint32_t seed = 0; 
	bool set_oq = false;
	bool use_oq = false;
	int nthreads = 0;
	std::string fixedinput = "";

	int opt = 0;
	int opt_idx = 0;
#ifndef NDEBUG
	std::string kmerlist("");
	std::string trustedlist("");
#endif
	while((opt = getopt_long(argc,argv,"k:usg:c:f:a:t:d:",long_options, &opt_idx)) != -1){
		switch(opt){
			case 'k':
				k = std::stoi(std::string(optarg));
				if(k <= 0 || k > KBBQ_MAX_KMER){
					std::cerr << put_now << "  Error: k must be <= " << KBBQ_MAX_KMER << " and > 0." << std::endl;
				}
				break;
			case 'u':
				use_oq = true;
				break;
			case 's':
				set_oq = true;
				break;
			case 'g':
				genomelen = std::stoull(std::string(optarg));
				break;
			case 'c':
				coverage = std::stoul(std::string(optarg));
				break;
			case 'f':
				fixedinput = std::string(optarg);
				break;
			case 'a':
				alpha = std::stold(std::string(optarg));
				break;
			case 't':
				nthreads = std::stoi(std::string(optarg));
				if(nthreads < 0){
					std::cerr << put_now << " Error: threads must be >= 0." << std::endl;
				}
				break;
#ifndef NDEBUG
			case 'd': {
				std::string optstr(optarg);
				std::istringstream stream(optstr);
				std::getline(stream, kmerlist, ',');
				std::getline(stream, trustedlist, ',');
				break;
			}
#endif
			case '?':
			default:
				std::cerr << put_now << "  Unknown argument " << (char)opt << std::endl;
				return 1;
				break;
		}
	}

	std::string filename("-");
	if(optind < argc){
		filename = std::string(argv[optind]);
		while(++optind < argc){
			std::cerr << put_now << " Warning: Extra argument " << argv[optind] << " ignored." << std::endl;
		}
	}

	long double sampler_desiredfpr = 0.01; //Lighter uses .01
	long double trusted_desiredfpr = 0.0005; // and .0005

	//create thread pool
	std::unique_ptr<htsThreadPool, std::function<void(htsThreadPool*)>> tp{
		new htsThreadPool,
		[](htsThreadPool* ptr){
			if(ptr->pool){hts_tpool_destroy(ptr->pool);}
		}
	};
	if(nthreads > 0 && !(tp->pool = hts_tpool_init(nthreads))){
		std::cerr << put_now << " Unable to construct thread pool." << std::endl;
		return 1;
	}


	//see if we have a bam
	htsFormat fmt;
	hFILE* fp = hopen(filename.c_str(), "r");
	if (hts_detect_format(fp, &fmt) < 0) {
		//error
		std::cerr << put_now << " Error opening file " << filename << std::endl;
		hclose_abruptly(fp);
		return 1;
	}
	bool is_bam = true;
	if(fmt.format == bam || fmt.format == cram){
		is_bam = true;
	} else if (fmt.format == fastq_format){
		is_bam = false;
	} else {
		//error
		std::cerr << put_now << " Error: File format must be bam, cram, or fastq." << std::endl;
		hclose_abruptly(fp);
		return 1;
	}
	std::unique_ptr<htsiter::HTSFile> file;
	covariateutils::CCovariateData data;

if(fixedinput == ""){ //no fixed input provided

	if(genomelen == 0){
		if(is_bam){
			std::cerr << put_now << " Estimating genome length" << std::endl;
			samFile* sf = hts_hopen(fp, filename.c_str(), "r");
			if(tp->pool && hts_set_thread_pool(sf, tp.get()) != 0){
				std::cerr << "Couldn't attach thread pool to file " << filename << std::endl;
			};
			sam_hdr_t* h = sam_hdr_read(sf);
			for(int i = 0; i < sam_hdr_nref(h); ++i){
				genomelen += sam_hdr_tid2len(h, i);
			}
			sam_hdr_destroy(h);
			hts_close(sf);
			if(genomelen == 0){
				std::cerr << put_now << " Header does not contain genome information." <<
					" Unable to estimate genome length; please provide it on the command line" <<
					" using the --genomelen option." << std::endl;
				return 1;
			} else {
				std::cerr << put_now << " Genome length is " << genomelen <<" bp." << std::endl;
			}
		} else {
			std::cerr << put_now << " Error: --genomelen must be specified if input is not a bam." << std::endl;
		}
	} else {
		if(hclose(fp) != 0){
			std::cerr << put_now << " Error closing file!" << std::endl;
		}
	}
	
	//alpha not provided, coverage not provided
	if(alpha == 0){
		std::cerr << put_now << " Estimating alpha." << std::endl;
		if(coverage == 0){
			std::cerr << put_now << " Estimating coverage." << std::endl;
			uint64_t seqlen = 0;
			file = std::move(open_file(filename, tp.get(), is_bam, use_oq, set_oq));
			std::string seq("");
			while((seq = file->next_str()) != ""){
				seqlen += seq.length();
			}
			if (seqlen == 0){
				std::cerr << put_now << " Error: total sequence length in file " << filename <<
					" is 0. Check that the file isn't empty." << std::endl;
				return 1;
			}
			std::cerr << put_now << " Total Sequence length: " << seqlen << std::endl;
			std::cerr << put_now << " Genome length: " << genomelen << std::endl;
			coverage = seqlen/genomelen;
			std::cerr << put_now << " Estimated coverage: " << coverage << std::endl;
			if(coverage == 0){
				std::cerr << put_now << " Error: estimated coverage is 0." << std::endl;
				return 1;
			}
		}
		alpha = 7.0l / (long double)coverage; // recommended by Lighter authors		
	}

	if(coverage == 0){ //coverage hasn't been estimated but alpha is given
		coverage = 7.0l/alpha;
	}

	file = std::move(open_file(filename, tp.get(), is_bam, use_oq, set_oq));

	std::cerr << put_now << " Sampling kmers at rate " << alpha << std::endl;

	//in the worst case, every kmer is unique, so we have genomelen * coverage kmers
	//then we will sample proportion alpha of those.
	unsigned long long int approx_kmers = genomelen*coverage*alpha;
	bloom::Bloom subsampled(approx_kmers, sampler_desiredfpr); //lighter uses 1.5 * genomelen
	bloom::Bloom trusted(approx_kmers, trusted_desiredfpr);

	if(seed == 0){
		seed = minion::create_seed_seq().GenerateOne();
	}
	std::cerr << put_now << " Seed: " << seed << std::endl ;

	//sample kmers here.
#ifdef KBBQ_USE_RAND_SAMPLER
	std::srand(seed); //lighter uses 17
#endif
	htsiter::KmerSubsampler subsampler(file.get(), k, alpha, seed);
	//load subsampled bf.
	//these are hashed kmers.
	recalibrateutils::subsample_kmers(subsampler, subsampled);

	//report number of sampled kmers
	std::cerr << put_now << " Sampled " << subsampled.inserted_elements() << " valid kmers." << std::endl;

#ifndef NDEBUG
	//ensure kmers are properly sampled
	if(kmerlist != ""){
		std::ifstream kmersin(kmerlist);
		bloom::Kmer kin(k);
		for(std::string line; std::getline(kmersin, line); ){
			kin.reset();
			for(char c: line){
				kin.push_back(c);
			}
			if(kin.valid()){
				assert(subsampled.query(kin));
			}
		}
	}
#endif


	//calculate thresholds
	long double fpr = subsampled.fprate();
	std::cerr << put_now << " Approximate false positive rate: " << fpr << std::endl;
	if(fpr > .15){
		std::cerr << put_now << " Error: false positive rate is too high. " <<
			"Increase genomelen parameter and try again." << std::endl;
		return 1;
	}

	long double p = bloom::calculate_phit(subsampled, alpha);
	std::vector<int> thresholds = covariateutils::calculate_thresholds(k, p);
#ifndef NDEBUG
	std::vector<int> lighter_thresholds = {0, 1, 2, 3, 4, 4, 5, 5, 6, 6,
		7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12, 13, 13, 14, 14, 15, 15, 15, 16, 16, 17};

	std::cerr << put_now << " Thresholds: [ " ;
	std::copy(thresholds.begin(), thresholds.end(), std::ostream_iterator<int>(std::cerr, " "));
	std::cerr << "]" << std::endl;
	std::cerr << put_now << " Lighter Th: [ " ;
	std::copy(lighter_thresholds.begin(), lighter_thresholds.end(), std::ostream_iterator<int>(std::cerr, " "));
	std::cerr << "]" << std::endl;
	assert(lighter_thresholds == thresholds);
#endif

	std::vector<long double> cdf = covariateutils::log_binom_cdf(k,p);

	std::cerr << put_now << " log CDF: [ " ;
	for(auto c : cdf){std::cerr << c << " ";}
	std::cerr << "]" << std::endl;

	//get trusted kmers bf using subsampled bf
	std::cerr << put_now << " Finding trusted kmers" << std::endl;

	file = std::move(open_file(filename, tp.get(), is_bam, use_oq, set_oq));
	recalibrateutils::find_trusted_kmers(file.get(), trusted, subsampled, thresholds, k);

#ifndef NDEBUG
// check that all kmers in trusted list are actually trusted in our list.
// it seems that lighter has quite a few hash collisions that end up making
// it trust slightly more kmers than it should

if(trustedlist != ""){
	std::ifstream kmersin(trustedlist);
	bloom::Kmer kin(k);
	for(std::string line; std::getline(kmersin, line); ){
		// std::cerr << "Trusted kmer: " << line << std::endl;
		kin.reset();
		for(char c: line){
			kin.push_back(c);
		}
		if(!trusted.query(kin)){
			std::cerr << "Trusted kmer not found!" << std::endl;
			std::cerr << "Line: " << line << std::endl;
			std::cerr << "Kmer: " << kin << std::endl;
		}
		assert(trusted.query(kin));
	}
}
#endif

	//use trusted kmers to find errors
	std::cerr << put_now << " Finding errors" << std::endl;
	file = std::move(open_file(filename, tp.get(), is_bam, use_oq, set_oq));
	data = recalibrateutils::get_covariatedata(file.get(), trusted, k);
} else { //use fixedfile to find errors
	std::cerr << put_now << " Using fixed file to find errors." << std::endl;
	file = std::move(open_file(filename, tp.get(), is_bam, use_oq, set_oq));
	std::unique_ptr<htsiter::HTSFile> fixedfile = std::move(open_file(fixedinput, tp.get(), is_bam, use_oq, set_oq));
	while(file->next() >= 0 && fixedfile->next() >= 0){
		readutils::CReadData read = file->get();
		readutils::CReadData fixedread = fixedfile->get();
		std::transform(read.seq.begin(), read.seq.end(), fixedread.seq.begin(),
			read.errors.begin(), std::not_equal_to<char>{});
		data.consume_read(read);
	}
}



	std::vector<std::string> rgvals(readutils::CReadData::rg_to_int.size(), "");
	for(auto i : readutils::CReadData::rg_to_int){
		rgvals[i.second] = i.first;
	}

#ifndef NDEBUG
	std::cerr << put_now << " Covariate data:" << std::endl;
	std::cerr << "rgcov:";
	for(int i = 0; i < data.rgcov.size(); ++i){ //rgcov[rg][0] = errors
		std::cerr << i << ": " << rgvals[i] << " {" << data.rgcov[i][0] << ", " << data.rgcov[i][1] << "}" << std::endl;
	}
	std::cerr << "qcov:" << "(" << data.qcov.size() << ")" << std::endl;
	for(int i = 0; i < data.qcov.size(); ++i){
		std::cerr << i << " " << rgvals[i] << "(" << data.qcov[i].size() << ")" << ": [";
		for(int j = 0; j < data.qcov[i].size(); ++j){
			if(data.qcov[i][j][1] != 0){
				std::cerr << j << ":{" << data.qcov[i][j][0] << ", " << data.qcov[i][j][1] << "} ";
			}
		}
		std::cerr << "]" << std::endl;
	}
#endif

	//recalibrate reads and write to file
	std::cerr << put_now << " Training model" << std::endl;
	covariateutils::dq_t dqs = data.get_dqs();


#ifndef NDEBUG
	std::cerr << put_now << " dqs:\n" << "meanq: ";
	print_vec<int>(dqs.meanq);
	std::cerr << "\nrgdq:" << std::endl;
	for(int i = 0; i < dqs.rgdq.size(); ++i){
		std::cerr << rgvals[i] << ": " << dqs.rgdq[i] << " (" << dqs.meanq[i] + dqs.rgdq[i] << ")" << std::endl;
	}
	std::cerr << "qscoredq:" << std::endl;
	for(int i = 0; i < dqs.qscoredq.size(); ++i){
		for(int j = 0; j < dqs.qscoredq[i].size(); ++j){
			if(data.qcov[i][j][1] != 0){
				std::cerr << rgvals[i] << ", " << "q = " << j << ": " << dqs.qscoredq[i][j] << " (" <<
					dqs.meanq[i] + dqs.rgdq[i] + dqs.qscoredq[i][j] << ") " << 
					data.qcov[i][j][1] << " " << data.qcov[i][j][0] << std::endl;
			}
		}
	}
	std::cerr << "cycledq:" << std::endl;
	for(int i = 0; i < dqs.cycledq.size(); ++i){
		for(int j = 0; j < dqs.cycledq[i].size(); ++j){
			if(data.qcov[i][j][1] != 0){
				for(size_t k = 0; k < dqs.cycledq[i][j].size(); ++k){
					for(int l = 0; l < dqs.cycledq[i][j][k].size(); ++l){
						std::cerr << rgvals[i] << ", " << "q = " << j << ", cycle = " << (k ? -(l+1) : l+1) << ": " << dqs.cycledq[i][j][k][l] << " (" <<
							dqs.meanq[i] + dqs.rgdq[i] + dqs.qscoredq[i][j] + dqs.cycledq[i][j][k][l] << ") " << 
							data.cycov[i][j][k][l][1] << " " << data.cycov[i][j][k][l][0] << std::endl;
					}
				}
			}
		}
	}
	std::cerr << "dinucdq:" << std::endl;
	for(int i = 0; i < dqs.dinucdq.size(); ++i){
		for(int j = 0; j < dqs.dinucdq[i].size(); ++j){
			if(data.qcov[i][j][1] != 0){
				for(size_t k = 0; k < dqs.dinucdq[i][j].size(); ++k){
					std::cerr << rgvals[i] << ", " << "q = " << j << ", dinuc = " << seq_nt16_str[seq_nt16_table['0' + (k >> 2)]] << seq_nt16_str[seq_nt16_table['0' + (k & 3)]] << ": " << dqs.dinucdq[i][j][k] << " (" <<
						dqs.meanq[i] + dqs.rgdq[i] + dqs.qscoredq[i][j] + dqs.dinucdq[i][j][k] << ") " << 
						data.dicov[i][j][k][1] << " " << data.dicov[i][j][k][0] << std::endl;
				}
			}
		}
	}
#endif

	std::cerr << put_now << " Recalibrating file" << std::endl;
	file = std::move(open_file(filename, tp.get(), is_bam, use_oq, set_oq));
	recalibrateutils::recalibrate_and_write(file.get(), dqs, "-");
	return 0;
}
