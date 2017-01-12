#include <cmath>
#include <iostream>
#include <list>
#include <string>
#include <tuple>
#include <vector>
#include <unordered_map>
#include "sam.h"
#include "common.hpp"
#include "annotation.hpp"
#include "fusions.hpp"

using namespace std;

void predict_fusion_strands(fusion_t& fusion) {

	// count the number of reads which imply that strand1 is positive/negative
	// strand2 can be inferred from strand1
	unsigned int strand1_forward = 0;
	unsigned int strand1_reverse = 0;

	for (auto split_read1 = fusion.split_read1_list.begin(); split_read1 != fusion.split_read1_list.end(); ++split_read1) {
		if (!(**split_read1)[SPLIT_READ].predicted_strand_ambiguous) {
			if ((**split_read1)[SPLIT_READ].predicted_strand == FORWARD) {
				++strand1_forward;
			} else {
				++strand1_reverse;
			}
		}
	}

	for (auto split_read2 = fusion.split_read2_list.begin(); split_read2 != fusion.split_read2_list.end(); ++split_read2) {
		if (!(**split_read2)[SUPPLEMENTARY].predicted_strand_ambiguous) {
			if ((**split_read2)[SUPPLEMENTARY].predicted_strand == FORWARD) {
				++strand1_forward;
			} else {
				++strand1_reverse;
			}
		}
	}

	for (auto discordant_mate = fusion.discordant_mate_list.begin(); discordant_mate != fusion.discordant_mate_list.end(); ++discordant_mate) {
		if (!(**discordant_mate)[MATE1].predicted_strand_ambiguous &&
		    (**discordant_mate).filter != FILTERS.at("hairpin")) { // skip discordant mates arising from hairpin structures, because they are usually ambiguous

			// find out which mate supports breakpoint1
			alignment_t* mate1 = &(**discordant_mate)[MATE1];
			alignment_t* mate2 = &(**discordant_mate)[MATE2];
			if (mate1->contig != fusion.contig1 || // it is clear which mate supports which breakpoint, when the contigs of the breakpoints are different
			    (mate1->strand == FORWARD) != /*xor*/ (fusion.direction1 == DOWNSTREAM)) { // or when the mates point in different directions
				swap(mate1, mate2);
			} else // determine association based on proximity to breakpoint
			if (mate1->strand == mate2->strand) {
				// if we get here, the mates are on the same contig and point in identical directions
				// => find out which is nearer to which breakpoint
				position_t mate1_end, mate2_end;
				if (fusion.direction1 == DOWNSTREAM) {
					mate1_end = mate1->end;
					mate2_end = mate2->end;
				} else {
					mate1_end = mate1->start;
					mate2_end = mate2->start;
				}
				unsigned int distance1 = abs(fusion.breakpoint1 - mate1_end) + abs(fusion.breakpoint2 - mate2_end);
				unsigned int distance2 = abs(fusion.breakpoint2 - mate1_end) + abs(fusion.breakpoint1 - mate2_end);
				if (distance1 == distance2) { // it's a tie => unclear which mate supports which breakpoint
					continue;
				} else if (distance2 < distance1) { // mate1 is closer to breakpoint2 and mate2 closer to breakpoint1 => swap
					swap(mate1, mate2);
				}
			}

			if (mate1->predicted_strand == FORWARD) {
				++strand1_forward;
			} else {
				++strand1_reverse;
			}
		}
	}

	if (strand1_forward == strand1_reverse) { // there are as many reads supporting strand1==FORWARD as there are reads supporting strand1==REVERSE
		fusion.predicted_strands_ambiguous = true;
	} else {
		fusion.predicted_strands_ambiguous = false;
		fusion.predicted_strand1 = (strand1_forward > strand1_reverse) ? FORWARD : REVERSE;
		fusion.predicted_strand2 = complement_strand_if(fusion.predicted_strand1, fusion.direction1 == fusion.direction2);
	}

}

// try to determine which gene makes the 5' end of the transcript by
// looking at which promoter likely drives transcription
void predict_transcript_start(fusion_t& fusion) {

	fusion.transcript_start_ambiguous = false;

	if (fusion.spliced1 ||
	    !fusion.predicted_strands_ambiguous && fusion.predicted_strand1 == fusion.gene1->strand) {

		if (fusion.gene1->strand == FORWARD && fusion.direction1 == DOWNSTREAM) {
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else if (fusion.gene1->strand == FORWARD && fusion.direction1 == UPSTREAM) {
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		} else if (fusion.gene1->strand == REVERSE && fusion.direction1 == UPSTREAM) {
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else { // fusion.gene1->strand == REVERSE && fusion.direction1 == DOWNSTREAM
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		}

	} else if (fusion.spliced2 ||
	           !fusion.predicted_strands_ambiguous && fusion.predicted_strand2 == fusion.gene2->strand) {

		if (fusion.gene2->strand == FORWARD && fusion.direction2 == DOWNSTREAM) {
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		} else if (fusion.gene2->strand == FORWARD && fusion.direction2 == UPSTREAM) {
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else if (fusion.gene2->strand == REVERSE && fusion.direction2 == UPSTREAM) {
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		} else { // fusion.gene2->strand == REVERSE && fusion.direction2 == DOWNSTREAM
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		}

	} else if (!fusion.exonic1 && !fusion.exonic2 ||
	           !fusion.predicted_strands_ambiguous) { // this can happen, if both strands could be predicted successfully, but were both predicted to be on opposite strands of the genes of the fusion

		fusion.transcript_start_ambiguous = true;

	} else // when we get here, the strands could not be predicted at all => make an educated guess based on whether the breakpoints are in exons or introns
	if (!fusion.exonic1 && fusion.exonic2) { // if breakpoint1 is intronic/intergenic, then gene2 has priority

		if (fusion.gene2->strand == FORWARD && fusion.direction2 == DOWNSTREAM) { // transcript = gene2(+) -> gene1(+/-)
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		} else if (fusion.gene2->strand == REVERSE && fusion.direction2 == UPSTREAM) { // transcript = gene2(-) -> gene1(+/-)
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		} else if (fusion.split_reads1 + fusion.split_reads2 == 0 && // no split reads => precise breakpoint unknown (could be spliced)
		           fusion.is_read_through() &&
		           (fusion.gene2->strand == FORWARD && fusion.direction2 == UPSTREAM ||
		            fusion.gene2->strand == REVERSE && fusion.direction2 == DOWNSTREAM)) {
				fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else { // ambiguous, since strand of intronic/intergenic region is unclear
			fusion.transcript_start_ambiguous = true;
		}

	} else if (!fusion.exonic2 && fusion.exonic1) { // if breakpoint1 is intronic/intergenic, then gene1 has priority

		if (fusion.gene1->strand == FORWARD && fusion.direction1 == DOWNSTREAM) { // transcript = gene1(+) -> gene2(+/-)
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else if (fusion.gene1->strand == REVERSE && fusion.direction1 == UPSTREAM) { // transcript = gene1(-) -> gene2(+/-)
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else if (fusion.split_reads1 + fusion.split_reads2 == 0 && // no split reads => precise breakpoint unknown (could be spliced)
		           fusion.is_read_through() &&
		           (fusion.gene1->strand == FORWARD && fusion.direction1 == UPSTREAM ||
		            fusion.gene1->strand == REVERSE && fusion.direction1 == DOWNSTREAM)) {
				fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else { // ambiguous, since strand of intronic/intergenic region is unclear
			fusion.transcript_start_ambiguous = true;
		}

	} else { // in all other cases gene1 has priority

		if (fusion.gene1->strand == FORWARD && fusion.direction1 == DOWNSTREAM || // transcript = gene1(+) -> gene2(+/-)
		    fusion.gene1->strand == REVERSE && fusion.direction1 == UPSTREAM) { // transcript = gene1(-) -> gene2(+/-)
			fusion.transcript_start = TRANSCRIPT_START_GENE1;
		} else if (fusion.gene2->strand == FORWARD && fusion.direction2 == DOWNSTREAM || // transcript = gene2(+) -> gene1(+/-)
		           fusion.gene2->strand == REVERSE && fusion.direction2 == UPSTREAM) { // transcript = gene2(-) -> gene1(+/-)
			fusion.transcript_start = TRANSCRIPT_START_GENE2;
		} else { // end-to-end-fused genes
			fusion.transcript_start_ambiguous = true;
		}

	}

	if (fusion.transcript_start_ambiguous) {
		fusion.transcript_start = TRANSCRIPT_START_GENE1; // this guarantees deterministic behavior and makes sure the transcript sequences are printed in correct order
}

	// predict strands from gene orientations, if they could not be predicted from splice patterns
	if (!fusion.transcript_start_ambiguous && fusion.predicted_strands_ambiguous) {
		fusion.predicted_strands_ambiguous = false;
		if (fusion.transcript_start == TRANSCRIPT_START_GENE1) {
			fusion.predicted_strand1 = fusion.gene1->strand;
			fusion.predicted_strand2 = complement_strand_if(fusion.predicted_strand1, fusion.direction1 == fusion.direction2);
		} else { // fusion.transcript_start == TRANSCRIPT_START_GENE2
			fusion.predicted_strand2 = fusion.gene2->strand;
			fusion.predicted_strand1 = complement_strand_if(fusion.predicted_strand2, fusion.direction1 == fusion.direction2);
		}
	}
}


unsigned int find_fusions(chimeric_alignments_t& chimeric_alignments, fusions_t& fusions, exon_annotation_index_t& exon_annotation_index, const int max_mate_gap) {

	typedef unordered_map< tuple<gene_t/*gene1*/,gene_t/*gene2*/>, vector<chimeric_alignments_t::iterator> > discordant_mates_by_gene_pair_t;
	discordant_mates_by_gene_pair_t discordant_mates_by_gene_pair; // contains the discordant mates for each pair of genes

	for (chimeric_alignments_t::iterator i = chimeric_alignments.begin(); i != chimeric_alignments.end(); ++i) {

		contig_t contig1, contig2;
		position_t breakpoint1, breakpoint2;
		direction_t direction1, direction2;
		gene_set_t genes1, genes2;
		bool exonic1, exonic2;
		position_t anchor_start1, anchor_start2;

		if (i->second.size() == 3) { // split read

			// extract info about fusion from alignments
			contig1 = i->second[SPLIT_READ].contig;
			contig2 = i->second[SUPPLEMENTARY].contig;
			breakpoint1 = (i->second[SPLIT_READ].strand == FORWARD) ? i->second[SPLIT_READ].start : i->second[SPLIT_READ].end;
			breakpoint2 = (i->second[SUPPLEMENTARY].strand == FORWARD) ? i->second[SUPPLEMENTARY].end : i->second[SUPPLEMENTARY].start;
			genes1 = i->second[SPLIT_READ].genes;
			genes2 = i->second[SUPPLEMENTARY].genes;
			direction1 = (i->second[SPLIT_READ].strand == FORWARD) ? UPSTREAM : DOWNSTREAM;
			direction2 = (i->second[SUPPLEMENTARY].strand == FORWARD) ? DOWNSTREAM : UPSTREAM;
			exonic1 = i->second[SPLIT_READ].exonic;
			exonic2 = i->second[SUPPLEMENTARY].exonic;
			anchor_start1 = (i->second[MATE1].strand == FORWARD) ? i->second[MATE1].start : i->second[MATE1].end;
			anchor_start2 = (i->second[SUPPLEMENTARY].strand == FORWARD) ? i->second[SUPPLEMENTARY].start : i->second[SUPPLEMENTARY].end;

			// make sure the breakpoint with the lower coordinate is always first
			// otherwise the same fusion could generate two entries in the fusions hashmap
			bool swapped = false;
			if (contig1 > contig2 || (contig1 == contig2 && breakpoint1 > breakpoint2)) {
				swap(contig1, contig2);
				swap(breakpoint1, breakpoint2);
				swap(genes1, genes2);
				swap(direction1, direction2);
				swap(exonic1, exonic2);
				swap(anchor_start1, anchor_start2);
				swapped = true;
			}

			// make a fusion from the given breakpoints
			bool overlap_duplicate1 = false;
			for (gene_set_t::iterator gene1 = genes1.begin(); gene1 != genes1.end(); ++gene1) {
				bool overlap_duplicate2 = false;
				for (gene_set_t::iterator gene2 = genes2.begin(); gene2 != genes2.end(); ++gene2) {

					// copy properties of supporting read to fusion
					fusion_t& fusion = fusions[make_tuple(*gene1, *gene2, contig1, contig2, breakpoint1, breakpoint2, direction1, direction2)];
					fusion.gene1 = *gene1; fusion.gene2 = *gene2;
					fusion.direction1 = direction1; fusion.direction2 = direction2;
					fusion.contig1 = contig1; fusion.contig2 = contig2;
					fusion.breakpoint1 = breakpoint1; fusion.breakpoint2 = breakpoint2;
					fusion.exonic1 = exonic1; fusion.exonic2 = exonic2;

					if (fusion.split_reads1 + fusion.split_reads2 + fusion.discordant_mates == 0) {
						if (i->second.filter == NULL)
							fusion.filter = NULL;
						else
							fusion.filter = i->second.filter;
					}

					// expand the size of the anchor
					if (fusion.direction1 == DOWNSTREAM && (anchor_start1 < fusion.anchor_start1 || fusion.anchor_start1 == 0)) {
						fusion.anchor_start1 = anchor_start1;
					} else if (fusion.direction1 == UPSTREAM && (anchor_start1 > fusion.anchor_start1 || fusion.anchor_start1 == 0)) {
						fusion.anchor_start1 = anchor_start1;
					}
					if (fusion.direction2 == DOWNSTREAM && (anchor_start2 < fusion.anchor_start2 || fusion.anchor_start2 == 0)) {
						fusion.anchor_start2 = anchor_start2;
					} else if (fusion.direction2 == UPSTREAM && (anchor_start2 > fusion.anchor_start2 || fusion.anchor_start2 == 0)) {
						fusion.anchor_start2 = anchor_start2;
					}

					// when the breakpoint falls into a region where genes overlap,
					// mark all genes except the first as "overlap_duplicate"
					fusion.overlap_duplicate1 = overlap_duplicate1;
					fusion.overlap_duplicate2 = overlap_duplicate2;
					overlap_duplicate2 = true;

					// increase split read counters for the given fusion
					if (swapped) {
						fusion.split_read2_list.push_back(&(i->second));
						if (i->second.filter == NULL)
							fusion.split_reads2++;
					} else {
						fusion.split_read1_list.push_back(&(i->second));
						if (i->second.filter == NULL)
							fusion.split_reads1++;
					}
				}
				overlap_duplicate1 = true;
			}

		} else if (i->second.size() == 2) { // discordant mates

			// extract info about fusion from alignments
			contig1 = i->second[MATE1].contig;
			contig2 = i->second[MATE2].contig;
			breakpoint1 = (i->second[MATE1].strand == FORWARD) ? i->second[MATE1].end : i->second[MATE1].start;
			breakpoint2 = (i->second[MATE2].strand == FORWARD) ? i->second[MATE2].end : i->second[MATE2].start;
			genes1 = i->second[MATE1].genes;
			genes2 = i->second[MATE2].genes;
			direction1 = (i->second[MATE1].strand == FORWARD) ? DOWNSTREAM : UPSTREAM;
			direction2 = (i->second[MATE2].strand == FORWARD) ? DOWNSTREAM : UPSTREAM;
			exonic1 = i->second[MATE1].exonic;
			exonic2 = i->second[MATE2].exonic;
			anchor_start1 = (i->second[MATE1].strand == FORWARD) ? i->second[MATE1].start : i->second[MATE1].end;
			anchor_start2 = (i->second[MATE2].strand == FORWARD) ? i->second[MATE2].start : i->second[MATE2].end;

			// make sure the breakpoint with the lower coordinate is always first
			// otherwise the same fusion could generate two entries in the fusions hashmap
			if (contig1 > contig2 || (contig1 == contig2 && breakpoint1 > breakpoint2)) {
				swap(contig1, contig2);
				swap(breakpoint1, breakpoint2);
				swap(genes1, genes2);
				swap(direction1, direction2);
				swap(exonic1, exonic2);
				swap(anchor_start1, anchor_start2);
			}

			// make a fusion from the given breakpoints
			bool overlap_duplicate1 = false;
			for (gene_set_t::iterator gene1 = genes1.begin(); gene1 != genes1.end(); ++gene1) {
				bool overlap_duplicate2 = false;
				for (gene_set_t::iterator gene2 = genes2.begin(); gene2 != genes2.end(); ++gene2) {

					// copy properties of supporting read to fusion
					bool is_new_fusion = fusions.find(make_tuple(*gene1, *gene2, contig1, contig2, breakpoint1, breakpoint2, direction1, direction2)) == fusions.end();
					fusion_t& fusion = fusions[make_tuple(*gene1, *gene2, contig1, contig2, breakpoint1, breakpoint2, direction1, direction2)];
					fusion.gene1 = *gene1; fusion.gene2 = *gene2;
					fusion.direction1 = direction1; fusion.direction2 = direction2;
					fusion.contig1 = contig1; fusion.contig2 = contig2;
					fusion.breakpoint1 = breakpoint1; fusion.breakpoint2 = breakpoint2;
					fusion.exonic1 = exonic1; fusion.exonic2 = exonic2;

					if (i->second.filter == NULL) {
						fusion.filter = NULL;
					} else if (is_new_fusion || fusion.filter != NULL) {
						fusion.filter = i->second.filter;
					}

					// expand the size of the anchor
					if (fusion.direction1 == DOWNSTREAM && (anchor_start1 < fusion.anchor_start1 || fusion.anchor_start1 == 0)) {
						fusion.anchor_start1 = anchor_start1;
					} else if (fusion.direction1 == UPSTREAM && (anchor_start1 > fusion.anchor_start1 || fusion.anchor_start1 == 0)) {
						fusion.anchor_start1 = anchor_start1;
					}
					if (fusion.direction2 == DOWNSTREAM && (anchor_start2 < fusion.anchor_start2 || fusion.anchor_start2 == 0)) {
						fusion.anchor_start2 = anchor_start2;
					} else if (fusion.direction2 == UPSTREAM && (anchor_start2 > fusion.anchor_start2 || fusion.anchor_start2 == 0)) {
						fusion.anchor_start2 = anchor_start2;
					}

					// when the breakpoint falls into a region where genes overlap,
					// mark all genes except the first as "overlap_duplicate"
					fusion.overlap_duplicate1 = overlap_duplicate1;
					fusion.overlap_duplicate2 = overlap_duplicate2;
					overlap_duplicate2 = true;

					// store the discordant mates in a hashmap for fast lookup
					// we will need this later to find all the discordant mates supporting a given fusion
					discordant_mates_by_gene_pair[make_tuple(*gene1, *gene2)].push_back(i);
				}
				overlap_duplicate1 = true;
			}
		}
	}

	// for each fusion, count the supporting discordant mates
//TODO count spliced split reads like discordant mates
	unsigned int subsampled_fusions = 0;
	for (fusions_t::iterator i = fusions.begin(); i != fusions.end(); ++i) {

		if (i->second.filter != NULL)
			continue; // don't look for discordant mates, if the fusion has been filtered

		// get list of discordant mates supporting a fusion between the given gene pair
		discordant_mates_by_gene_pair_t::iterator discordant_mates = discordant_mates_by_gene_pair.find(make_tuple(i->second.gene1, i->second.gene2));
		if (discordant_mates != discordant_mates_by_gene_pair.end()) {

			// discard those discordant mates which point in the wrong direction (away from the breakpoint)
			for (auto discordant_mate = discordant_mates->second.begin(); discordant_mate != discordant_mates->second.end(); ++discordant_mate) {

				alignment_t* mate1 = &((**discordant_mate).second[MATE1]); // introduce some aliases for cleaner code
				alignment_t* mate2 = &((**discordant_mate).second[MATE2]);

				// make sure mate1 points to the mate with the lower coordinate
				// this ensures that the coordinate of the correct mate is compared against the coordinate of the breakpoint
				position_t mate1_breakpoint = (mate1->strand == FORWARD) ? mate1->end : mate1->start;
				position_t mate2_breakpoint = (mate2->strand == FORWARD) ? mate2->end : mate2->start;
				if (mate1->contig > mate2->contig || mate1->contig == mate2->contig && mate1_breakpoint > mate2_breakpoint)
					swap(mate1, mate2);

				if (((i->second.direction1 == DOWNSTREAM && mate1->strand == FORWARD && (mate1->end-2 <= i->second.breakpoint1 && i->second.split_reads1 + i->second.split_reads2 > 0 || mate1->end-max_mate_gap <= i->second.breakpoint1 && i->second.split_reads1 + i->second.split_reads2 == 0)) || (i->second.direction1 == UPSTREAM && mate1->strand == REVERSE && (mate1->start+2 >= i->second.breakpoint1 && i->second.split_reads1+i->second.split_reads2 > 0 || mate1->start+max_mate_gap >= i->second.breakpoint1 && i->second.split_reads1+i->second.split_reads2 == 0))) &&
				    ((i->second.direction2 == DOWNSTREAM && mate2->strand == FORWARD && (mate2->end-2 <= i->second.breakpoint2 && i->second.split_reads1 + i->second.split_reads2 > 0 || mate2->end-max_mate_gap <= i->second.breakpoint2 && i->second.split_reads1 + i->second.split_reads2 == 0)) || (i->second.direction2 == UPSTREAM && mate2->strand == REVERSE && (mate2->start+2 >= i->second.breakpoint2 && i->second.split_reads1+i->second.split_reads2 > 0 || mate2->start+max_mate_gap >= i->second.breakpoint2 && i->second.split_reads1+i->second.split_reads2 == 0)))) {

					i->second.discordant_mate_list.push_back(&(**discordant_mate).second);

					if ((*discordant_mate)->second.filter == NULL)
						i->second.discordant_mates++;
					else if (i->second.filter != NULL)
						i->second.filter = (**discordant_mate).second.filter;

					// expand the size of the anchor
					if (i->second.direction1 == DOWNSTREAM && (mate1->start < i->second.anchor_start1 || i->second.anchor_start1 == 0)) {
						i->second.anchor_start1 = mate1->start;
					} else if (i->second.direction1 == UPSTREAM && (mate1->end > i->second.anchor_start1 || i->second.anchor_start1 == 0)) {
						i->second.anchor_start1 = mate1->end;
					}
					if (i->second.direction2 == DOWNSTREAM && (mate2->start < i->second.anchor_start2 || i->second.anchor_start2 == 0)) {
						i->second.anchor_start2 = mate2->start;
					} else if (i->second.direction2 == UPSTREAM && (mate2->end > i->second.anchor_start2 || i->second.anchor_start2 == 0)) {
						i->second.anchor_start2 = mate2->end;
					}

					if (i->second.discordant_mates >= 1000) {
						subsampled_fusions++;
						break;
					}
				}
			}
		}
	}

	if (subsampled_fusions > 0)
		cerr << "WARNING: " << subsampled_fusions << " fusions were subsampled, because they have more than 1000 discordant mates" << endl;

	unsigned int remaining = 0;
	for (fusions_t::iterator fusion = fusions.begin(); fusion != fusions.end(); ++fusion) {

		// predict strands from predicted strands of supporting reads
		predict_fusion_strands(fusion->second);

		// check if breakpoints are at splice-sites
		// (must come after strand prediction)
		if (fusion->second.split_read1_list.size() + fusion->second.split_read2_list.size() == 0 || // fusions with only discordant mates cannot be spliced
		    fusion->second.predicted_strands_ambiguous) {
			fusion->second.spliced1 = false;
			fusion->second.spliced2 = false;
		} else {
			fusion->second.spliced1 = fusion->second.exonic1 &&
			                          fusion->second.gene1->strand == fusion->second.predicted_strand1 &&
			                          is_breakpoint_spliced(fusion->second.gene1, fusion->second.direction1, fusion->second.contig1, fusion->second.breakpoint1, exon_annotation_index);
			fusion->second.spliced2 = fusion->second.exonic2 &&
			                          fusion->second.gene2->strand == fusion->second.predicted_strand2 &&
			                          is_breakpoint_spliced(fusion->second.gene2, fusion->second.direction2, fusion->second.contig2, fusion->second.breakpoint2, exon_annotation_index);
		}

		// predict which gene makes the 5' end from strands or splice-sites or gene orientations
		// (must come after splice-site prediction)
		predict_transcript_start(fusion->second);

		// count fusions which have at least one non-filtered read
		if (fusion->second.filter == NULL)
			remaining++;
	}

	return remaining;
}

