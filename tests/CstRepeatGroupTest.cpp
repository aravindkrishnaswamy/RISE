//////////////////////////////////////////////////////////////////////
//
//  CstRepeatGroupTest.cpp - Facet 1 / #5 slice 1: the RepeatGroup VIEW (D3).
//  A repeatable parameter's occurrences are exposed as a read-through, document-
//  ordered snapshot (DocRepeatGroup) with per-element name-path addressing
//  (DocRepeatElementId -> `chunk.role[k]`).  The view is READ-ONLY over the CST
//  (no apply / derive / grammar change), so it cannot perturb the lossless round-trip.
//  Locks in:
//    [identity]   a repeatable-param scene (incl. interleaved blank trivia) round-trips
//                 byte-for-byte after building the view.
//    [view]       DocRepeatGroup returns ALL occurrences in DOCUMENT order; counts match;
//                 interleaved trivia is NOT an occurrence.
//    [addressing] DocRepeatElementId(k) == DocParamId(k) + the right Param node; out-of-range -> 0.
//    [order]      editing occ=k targets the k-th occurrence in document order.
//    [stability]  a value edit preserves every occurrence's NodeId + the count + the round-trip (D15).
//
//////////////////////////////////////////////////////////////////////

#include "../src/Library/Cst/Cst.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace RISE;
using namespace RISE::Cst;

static int g_pass = 0, g_fail = 0;
static void Check( bool c, const char* w ) { if( c ) ++g_pass; else { ++g_fail; std::printf( "  FAIL: %s\n", w ); } }

int main()
{
	std::printf( "CstRepeatGroupTest -- Facet 1 / #5 slice 1: RepeatGroup view (D3)\n" );

	// A repeatable-param chunk: piecewise_linear_function with 3 `cp` rows + an interleaved blank line.
	const std::string scene =
		"RISE ASCII SCENE 6\n"
		"piecewise_linear_function\n"
		"{\n"
		"name f\n"
		"cp 0 0\n"
		"\n"                      // blank trivia between occurrences -- NOT a 'cp', must round-trip
		"cp 0.5 1\n"
		"cp 1 0\n"
		"}\n";
	Document doc = ParseToCst( scene );
	const NodeId fId = DocFindByName( doc, "piecewise_linear_function/f" );
	Check( fId != 0, "located the piecewise_linear_function chunk by name" );

	// [identity] building the view must not perturb the tree.
	Check( SerializeCst( doc ) == scene, "identity: repeatable-param scene round-trips byte-for-byte" );

	// [view] all 3 cp occurrences; counts match; blank trivia skipped.
	RepeatGroupView v = DocRepeatGroup( doc, fId, "cp" );
	Check( v.chunkId == fId && v.role == "cp", "view: chunkId + role populated" );
	Check( v.occurrences.size() == 3 && v.occurrenceIds.size() == 3, "view: 3 cp occurrences (interleaved blank trivia NOT counted)" );
	Check( DocRepeatCount( doc, fId, "cp" ) == 3, "count: DocRepeatCount(cp) == 3" );
	Check( DocRepeatCount( doc, fId, "name" ) == 1, "count: a non-repeated param (name) == 1" );
	Check( DocRepeatCount( doc, fId, "nonesuch" ) == 0, "count: an absent param == 0" );

	// [view] document order + id parity with the existing per-occurrence index.
	bool orderOk = true, distinctOk = true;
	for( int k = 0; k < 3; ++k ) {
		if( v.occurrenceIds[k] != DocParamId( doc, fId, "cp", k ) ) orderOk = false;
		if( v.occurrenceIds[k] == 0 ) distinctOk = false;
		for( int j = 0; j < k; ++j ) if( v.occurrenceIds[k] == v.occurrenceIds[j] ) distinctOk = false;
	}
	Check( orderOk, "view: occurrenceIds[k] == DocParamId(cp,k) for all k (DOCUMENT order)" );
	Check( distinctOk, "view: the 3 occurrence ids are non-zero + distinct" );

	// [addressing] cp[k] resolves to the view's k-th occurrence (id + Param node); out-of-range -> 0.
	bool addrOk = true;
	for( int k = 0; k < 3; ++k ) {
		NodeRef node;
		if( DocRepeatElementId( doc, fId, "cp", k, &node ) != v.occurrenceIds[k] ) addrOk = false;
		if( !node || node.get() != v.occurrences[k].get() ) addrOk = false;
	}
	Check( addrOk, "addressing: DocRepeatElementId(cp,k) == view id + the k-th Param node, all k" );
	Check( DocRepeatElementId( doc, fId, "cp", -1 ) == 0, "addressing: a negative index -> 0" );
	Check( DocRepeatElementId( doc, fId, "cp", 3 ) == 0, "addressing: index == count -> 0 (out of range)" );
	{ NodeRef n; DocRepeatElementId( doc, fId, "cp", 9, &n ); Check( !n, "addressing: out-of-range leaves outParam null" ); }
	Check( DocRepeatElementId( doc, fId, "nonesuch", 0 ) == 0, "addressing: an absent role -> 0" );

	// [order] editing occ=0 changes the FIRST cp (document order); the rest intact.
	{
		Document d0 = DocSetParamValue( doc, fId, "cp", 0, "9 9" );
		Check( SerializeCst( d0 ).find( "cp 9 9\n\ncp 0.5 1\ncp 1 0" ) != std::string::npos,
		       "order: editing cp[0] changes the FIRST occurrence, others intact" );
	}

	// [stability D15] a value edit on cp[1] preserves EVERY occurrence's NodeId + count + round-trip.
	{
		NodeId before[3];
		for( int k = 0; k < 3; ++k ) before[k] = DocParamId( doc, fId, "cp", k );
		Document d1 = DocSetParamValue( doc, fId, "cp", 1, "7 7" );
		const NodeId fId1 = DocFindByName( d1, "piecewise_linear_function/f" );
		bool idsStable = ( fId1 == fId && fId1 != 0 );
		for( int k = 0; k < 3; ++k ) if( DocParamId( d1, fId1, "cp", k ) != before[k] ) idsStable = false;
		Check( idsStable, "stability: a value edit on cp[1] preserves the chunk + all cp NodeIds (D15)" );
		Check( DocRepeatCount( d1, fId1, "cp" ) == 3, "stability: the count is unchanged after the edit" );
		Check( SerializeCst( d1 ).find( "cp 0 0\n\ncp 7 7\ncp 1 0" ) != std::string::npos,
		       "stability: the cp[1] edit hit the 2nd occurrence + round-trips" );
	}

	std::printf( "%d passed, %d failed.\n", g_pass, g_fail );
	return g_fail == 0 ? 0 : 1;
}
