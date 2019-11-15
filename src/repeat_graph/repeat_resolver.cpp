//(c) 2016 by Authors
//This file is a part of ABruijn program.
//Released under the BSD license (see LICENSE file)

#include <cmath>


#include "repeat_resolver.h"
#include "graph_processing.h"
#include "../common/config.h"
#include "../common/utils.h"
#include "../common/parallel.h"
#include "../common/disjoint_set.h"

#include <lemon/list_graph.h>
#include <lemon/matching.h>



//Resolves all repeats simulateously through the graph mathcing optimization,
//Given the reads connecting unique edges (or pairs of edges in the transitions graph)
int RepeatResolver::resolveConnections(const std::vector<Connection>& connections, 
									   float minSupport)
{
	std::unordered_map<FastaRecord::Id, 
					   std::vector<const Connection*>> connectIndex;
	for (auto& conn : connections)
	{
		connectIndex[conn.path.front()->edgeId].push_back(&conn);
		connectIndex[conn.path.front()->edgeId.rc()].push_back(&conn);
		connectIndex[conn.path.back()->edgeId].push_back(&conn);
		connectIndex[conn.path.back()->edgeId.rc()].push_back(&conn);
	}

	//Constructs transitions graph using the lemon library
	std::unordered_map<FastaRecord::Id, int> leftCoverage;
	std::unordered_map<FastaRecord::Id, int> rightCoverage;

	std::unordered_map<FastaRecord::Id, int> asmToLemon;
	std::unordered_map<int, FastaRecord::Id> lemonToAsm;
	lemon::ListGraph graph;
	lemon::ListGraph::EdgeMap<int> edgeWeights(graph);

	auto getEdge = [&graph](lemon::ListGraph::Node n1, lemon::ListGraph::Node n2)
	{
		for (lemon::ListGraph::IncEdgeIt edgeIt(graph, n1); 
			 edgeIt != lemon::INVALID; ++edgeIt) 
		{
			if (graph.oppositeNode(n1, edgeIt) == n2) return edgeIt;
		}
		return lemon::ListGraph::IncEdgeIt(lemon::INVALID);
	};

	for (auto& conn : connections)
	{
		GraphEdge* leftEdge = conn.path.front();
		GraphEdge* rightEdge = conn.path.back();

		if (leftEdge->edgeId == rightEdge->edgeId ||
			leftEdge->edgeId == rightEdge->edgeId.rc()) continue;

		++leftCoverage[leftEdge->edgeId];
		++rightCoverage[rightEdge->edgeId.rc()];

		if (!asmToLemon.count(leftEdge->edgeId))
		{
			auto newNode = graph.addNode();
			asmToLemon[leftEdge->edgeId] = graph.id(newNode);
			lemonToAsm[graph.id(newNode)] = leftEdge->edgeId;
		}
		if (!asmToLemon.count(rightEdge->edgeId.rc()))
		{
			auto newNode = graph.addNode();
			asmToLemon[rightEdge->edgeId.rc()] = graph.id(newNode);
			lemonToAsm[graph.id(newNode)] = rightEdge->edgeId.rc();
		}

		auto leftLemonNode = graph.nodeFromId(asmToLemon[leftEdge->edgeId]);
		auto rightLemonNode = graph.nodeFromId(asmToLemon[rightEdge->edgeId.rc()]);
		if (!graph.valid(getEdge(leftLemonNode, rightLemonNode)))
		{
			auto edge = graph.addEdge(leftLemonNode, rightLemonNode);
			edgeWeights[edge] = 0;
		}
		auto edge = getEdge(leftLemonNode, rightLemonNode);
		++edgeWeights[edge];
	}

	//copmutes maximum weight matching on this graph
	lemon::MaxWeightedMatching<lemon::ListGraph> matcher(graph, edgeWeights);
	matcher.run();

	//converting matching to the resolved paths on the graph
	std::unordered_set<FastaRecord::Id> usedEdges;
	std::vector<Connection> uniqueConnections;
	int unresolvedLinks = 0;
	for (auto lemonAsm : lemonToAsm)
	{
		auto mateNode = matcher.mate(graph.nodeFromId(lemonAsm.first));
		if (mateNode == lemon::INVALID) continue;

		FastaRecord::Id leftId = lemonAsm.second;
		FastaRecord::Id rightId = lemonToAsm[graph.id(mateNode)];
		int support = edgeWeights[getEdge(graph.nodeFromId(lemonAsm.first), 
										  mateNode)];

		if (usedEdges.count(leftId)) continue;
		usedEdges.insert(rightId);

		float confidence = (float)support / (leftCoverage[leftId] + 
									  		 rightCoverage[rightId]);

		Logger::get().debug() << "\tConnection " 
			<< leftId.signedId() << "\t" << rightId.rc().signedId()
			<< "\t" << support / 4 << "\t" << confidence;

		if (confidence < minSupport)
		{
			++unresolvedLinks;
			continue;
		}

		std::vector<Connection> spanningConnections;
		for (auto& conn : connectIndex[leftId])
		{
			if ((conn->path.front()->edgeId == leftId && 
				 	conn->path.back()->edgeId == rightId.rc()) ||
				(conn->path.front()->edgeId == rightId && 
				 	conn->path.back()->edgeId == leftId.rc()))
			{
				spanningConnections.push_back(*conn);
			}
		}
		if (spanningConnections.empty())
		{
			Logger::get().warning() << "Empty spanning connections";
			continue;
		}
		std::sort(spanningConnections.begin(), spanningConnections.end(),
				  [](const Connection c1, const Connection c2)
				{return c1.readSeq.length() < c2.readSeq.length();});
		uniqueConnections
			.push_back(spanningConnections[spanningConnections.size() / 2]);
	}

	//separates the resolved paths in the graph
	for (auto& conn : uniqueConnections)
	{
		FastaRecord::Id edgeId = _graph.newEdgeId();

		std::stringstream ss;
		ss << "edge_" << edgeId.signedId() << "_0_" 
			<< _readSeqs.getRecord(conn.readSeq.readId).description << "_"
			<< conn.readSeq.start << "_" << conn.readSeq.end;
		EdgeSequence edgeSeq = 
			_graph.addEdgeSequence(_readSeqs.getSeq(conn.readSeq.readId),
								   conn.readSeq.start, conn.readSeq.length(),
								   ss.str());

		_graph.separatePath(conn.path, edgeSeq, edgeId);
		_graph.separatePath(_graph.complementPath(conn.path), 
						    edgeSeq.complement(), edgeId.rc());
	}

	Logger::get().debug() << "[SIMPL] Resolved repeats: " << uniqueConnections.size();
	Logger::get().debug() << "RR links: " << connections.size() / 2;
	Logger::get().debug() << "Unresolved: " << unresolvedLinks;

	return uniqueConnections.size();
}

bool RepeatResolver::checkForTandemCopies(const GraphEdge* checkEdge,
										  const std::vector<GraphAlignment>& alignments)
{
	const int NEEDED_READS = 5;
	int readEvidence = 0;
	for (const auto& aln: alignments)
	{
		int numCopies = 0;
		//only copies fully covered by reads
		for (size_t i = 1; i < aln.size() - 1; ++i)
		{
			if (aln[i].edge == checkEdge) ++numCopies;
		}
		if (numCopies > 1) ++readEvidence;
	}
	return readEvidence >= NEEDED_READS;
}

bool RepeatResolver::checkByReadExtension(const GraphEdge* checkEdge,
										  const std::vector<GraphAlignment>& alignments)
{
	std::unordered_map<GraphEdge*, std::vector<int>> outFlanks;
	std::unordered_map<GraphEdge*, std::vector<int>> outSpans;
	int lowerBound = 0;
	for (auto& aln : alignments)
	{ 
		bool passedStart = false;
		int leftFlank = 0;
		int leftCoord = 0;
		bool foundUnique = false;
		for (size_t i = 0; i < aln.size(); ++i)
		{
			 //only high quality flanking alignments
			//if (aln[i].overlap.seqDivergence > 0.15) continue;

			if (!passedStart && aln[i].edge == checkEdge)
			{
				passedStart = true;
				leftFlank = aln[i].overlap.curEnd - aln[0].overlap.curBegin;
				leftCoord = aln[i].overlap.curEnd;
				continue;
			}
			if (passedStart && !aln[i].edge->repetitive)
			{
				if (aln[i].edge->edgeId != checkEdge->edgeId &&
					aln[i].edge->edgeId != checkEdge->edgeId.rc())
				{
					int rightFlank = aln.back().overlap.curEnd -
									 aln[i].overlap.curBegin;
					int alnSpan = aln[i].overlap.curBegin - leftCoord;
					outFlanks[aln[i].edge].push_back(std::min(leftFlank, rightFlank));
					outSpans[aln[i].edge].push_back(alnSpan);
				}
				foundUnique = true;
				break;
			}
		}
		if (!foundUnique)
		{
			lowerBound = std::max(lowerBound, 
								  aln.back().overlap.curBegin - leftCoord);
		}
	}

	//check if there is agreement
	int maxSupport = 0;
	for (auto& outConn : outFlanks)
	{
		if (maxSupport < (int)outConn.second.size())
		{
			maxSupport = outConn.second.size();
		}
	}

	int uniqueMult = 0;
	int minSupport = maxSupport / (int)Config::get("out_paths_ratio");
	//if there is at least one extension supported by more than 1 read,
	//make minimum support at least 1
	if (maxSupport > 1) minSupport = std::max(minSupport, 1);

	for (auto& outConn : outFlanks) 
	{
		if ((int)outConn.second.size() > minSupport)
		{
			++uniqueMult;
		}
	}
	
	if (uniqueMult > 1) 
	{
		Logger::get().debug() << "Starting " 
			<< checkEdge->edgeId.signedId() << " aln:" << alignments.size()
			<< " minSpan:" << lowerBound;
		for (auto& outEdgeCount : outFlanks)
		{
			int maxFlank = *std::max_element(outEdgeCount.second.begin(),
											 outEdgeCount.second.end());
			int minSpan = *std::min_element(outSpans[outEdgeCount.first].begin(),
											outSpans[outEdgeCount.first].end());

			std::string star = outEdgeCount.first->repetitive ? "R" : " ";
			std::string loop = outEdgeCount.first->isLooped() ? "L" : " ";
			std::string tip = outEdgeCount.first->isRightTerminal() ? "T" : " ";
			Logger::get().debug() << "\t" << star << " " << loop << " " << tip << " "
				<< outEdgeCount.first->edgeId.signedId() << "\tnum:" << outEdgeCount.second.size()
				<< "\tflank:" << maxFlank << "\tspan:" << minSpan;
		}
		return true;
	}
	return false;
}

//Classifies all edges into unique and repetitive based on the coverage + 
//alignment information - one of the key steps here.
void RepeatResolver::findRepeats()
{
	Logger::get().debug() << "Finding repeats";

	auto alnIndex = _aligner.makeAlignmentIndex();

	//all edges are unique at the beginning
	for (auto& edge : _graph.iterEdges())
	{
		edge->repetitive = false;
	}

	//Will operate on unbranching paths rather than single edges
	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();
	std::unordered_map<FastaRecord::Id, UnbranchingPath*> idToPath;
	for (auto& path : unbranchingPaths) idToPath[path.id] = &path;
	auto complPath = [&idToPath](UnbranchingPath* path)
	{
		if (idToPath.count(path->id.rc()))
		{
			return idToPath[path->id.rc()];
		}
		return path;	//self-complement
	};
	auto markRepetitive = [](UnbranchingPath* path)
	{
		for (auto& edge : path->path) edge->repetitive = true;
	};

	//first simlplier conditions without read alignment
	for (auto& path : unbranchingPaths)
	{
		if (!path.id.strand()) continue;

		//mark paths with high coverage as repetitive
		if (!Parameters::get().unevenCoverage &&
			path.meanCoverage > _multInf.getUniqueCovThreshold())
		{
			markRepetitive(&path);
			markRepetitive(complPath(&path));
			Logger::get().debug() << "High-cov: " 
				<< path.edgesStr() << "\t" << path.length << "\t" 
				<< path.meanCoverage;
		}

		//don't trust short loops, since they might contain unglued tandem
		//repeat variations
		const int MIN_RELIABLE_LOOP = 5000;
		if (path.isLooped() && path.length < MIN_RELIABLE_LOOP)
		{
			markRepetitive(&path);
			markRepetitive(complPath(&path));
			Logger::get().debug() << "Short-loop: " << path.edgesStr();
		}

		//mask self-complements
		for (auto& edge : path.path)
		{
			if (edge->selfComplement)
			{
				markRepetitive(&path);
				markRepetitive(complPath(&path));
				Logger::get().debug() << "Self-compl: " << path.edgesStr();
				break;
			}
		}

		//mask haplo-edges so they don't mess up repeat resolution
		for (auto& edge : path.path)
		{
			if (edge->altHaplotype)
			{
				markRepetitive(&path);
				markRepetitive(complPath(&path));
				Logger::get().debug() << "Haplo-edge: " << path.edgesStr();
				break;
			}
		}

		//mask unreliable edges with low coverage
		for (auto& edge : path.path)
		{
			if (edge->unreliable)
			{
				markRepetitive(&path);
				markRepetitive(complPath(&path));
				Logger::get().debug() << "Unreliable: " << path.edgesStr();
				break;
			}
		}

		//mask edges that appear multiple times within single reads
		for (auto& edge : path.path)
		{
			if (!edge->repetitive && this->checkForTandemCopies(edge, alnIndex[edge]))
			{
				markRepetitive(&path);
				markRepetitive(complPath(&path));
				Logger::get().debug() << "Tandem: " << path.edgesStr();
				break;
			}
		}
	}

	//Finally, using the read alignments
	//order might be important, process short edges first
	std::vector<UnbranchingPath*> sortedPaths;
	for (auto& path : unbranchingPaths) sortedPaths.push_back(&path);
	std::sort(sortedPaths.begin(), sortedPaths.end(),
			  [](const UnbranchingPath* p1, const UnbranchingPath* p2) 
			  {return p1->length < p2->length;});

	//in the case of metagenome do 2 passes, since some small
	//edges might not be detected from the 1st iteration
	//if tey are partes of mosaic repeats. In the case of
	//uniform coverage, this edges are typically detected using coverage
	size_t numIters = !Parameters::get().unevenCoverage ? 1 : 2;
	for (size_t i = 0; i < numIters; ++i)
	{
		Logger::get().debug() << "Repeat detection iteration " << i + 1;
		for (auto& path : sortedPaths)
		{
			if (!path->id.strand()) continue;
			if (path->path.front()->repetitive) continue;

			//be more aggressive in metagenome mode: assume that edges longer than 50k
			//are unique
			//if (Parameters::get().unevenCoverage &&
			//	path->length > (int)Config::get("unique_edge_length")) continue;

			bool rightRepeat = 
				this->checkByReadExtension(path->path.back(), 
										   alnIndex[path->path.back()]);
			bool leftRepeat = 
				this->checkByReadExtension(complPath(path)->path.back(), 
										   alnIndex[complPath(path)->path.back()]);
			if (rightRepeat || leftRepeat)
			{
				markRepetitive(path);
				markRepetitive(complPath(path));
				
				Logger::get().debug() << "Mult: " 
					<< path->edgesStr() << "\t" << path->length << "\t" 
					<< path->meanCoverage << "\t" " ("
					<< leftRepeat << "," << rightRepeat << ")";
			}
		}
	}

	//propagate repetitiveness through linked edges (flanking haplotype bubbles)
	for (GraphEdge* edge : _graph.iterEdges())
	{
		if (!edge->repetitive) continue;

		GraphEdge* curEdge = edge;
		for (;;)
		{
			curEdge->repetitive = true;
			if (curEdge->nodeRight->inEdges.size() == 1 &&
				curEdge->nodeRight->outEdges.size() == 1 &&
				!curEdge->nodeRight->outEdges[0]->repetitive)
			{
				curEdge = curEdge->nodeRight->outEdges[0];
			}
			else if (curEdge->rightLink && !curEdge->rightLink->repetitive)
			{
				curEdge = curEdge->rightLink;
			}
			else
			{
				break;
			}
		}
		curEdge = edge;
		for (;;)
		{
			curEdge->repetitive = true;
			if (curEdge->nodeLeft->inEdges.size() == 1 &&
				curEdge->nodeLeft->outEdges.size() == 1 &&
				!curEdge->nodeLeft->inEdges[0]->repetitive)
			{
				curEdge = curEdge->nodeLeft->inEdges[0];
			}
			else if (curEdge->leftLink && !curEdge->leftLink->repetitive)
			{
				curEdge = curEdge->leftLink;
			}
			else
			{
				break;
			}
		}
	}
}

void RepeatResolver::finalizeGraph()
{
	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();
	for (auto& path : unbranchingPaths)
	{
		if (!path.id.strand()) continue;

		bool highCoverage = (float)path.meanCoverage > 
							_multInf.getUniqueCovThreshold();

		if (!path.path.front()->selfComplement &&
			path.path.front()->repetitive &&
			path.length > (int)Config::get("unique_edge_length") &&
			(Parameters::get().unevenCoverage || !highCoverage))
		{
			for (auto& edge : path.path)
			{
				edge->repetitive = false;
				_graph.complementEdge(edge)->repetitive = false;
			}

			Logger::get().debug() << "Fixed: " 
				<< path.edgesStr() << "\t" << path.length << "\t" 
				<< path.meanCoverage;
		}
	}

	//apply coverage substractions that were made during repeat resolution
	for (auto& path : unbranchingPaths)
	{
		if (path.isLooped()) continue;
		for (auto& edge : path.path)
		{
			edge->meanCoverage = std::max(0, (int)edge->meanCoverage - 
											  _substractedCoverage[edge]);
		}
	}
}

//Iterates repeat detection and resolution until
//no new repeats are resolved
/*void RepeatResolver::resolveRepeats()
{
	const float MIN_SUPPORT = Config::get("min_repeat_res_support");
	while (true)
	{
		auto connections = this->getConnections();
		int resolvedConnections = 
			this->resolveConnections(connections, MIN_SUPPORT);

		this->clearResolvedRepeats();
		_multInf.trimTips();
		this->findRepeats();
		
		if (!resolvedConnections) break;
	}

	GraphProcessor proc(_graph, _asmSeqs);
	proc.fixChimericJunctions();
	_aligner.updateAlignments();
}*/

int RepeatResolver::resolveRepeats()
{
	const float MIN_SUPPORT = Config::get("min_repeat_res_support");

	auto connections = this->getConnections();
	int resolvedConnections = 
		this->resolveConnections(connections, MIN_SUPPORT);
	this->clearResolvedRepeats();

	GraphProcessor proc(_graph, _asmSeqs);
	proc.fixChimericJunctions();
	_aligner.updateAlignments();

	return resolvedConnections;
}


//extracts conenctions between pairs of unique edges from
//read alignments
std::vector<RepeatResolver::Connection> 
	RepeatResolver::getConnections()
{
	
	auto safeEdge = [this](GraphEdge* edge)
	{
		return !edge->isRepetitive();
	};

	int totalSafe = 0;
	for (GraphEdge* edge : _graph.iterEdges())
	{
		if (edge->edgeId.strand() && safeEdge(edge)) ++totalSafe;
	}
	Logger::get().debug() << "Total unique edges: " << totalSafe;

	const int32_t MAGIC_100 = 100;
	std::vector<Connection> readConnections;
	for (auto& readPath : _aligner.getAlignments())
	{
		GraphAlignment currentAln;
		int32_t readStart = 0;
		for (auto& aln : readPath)
		{
			if (currentAln.empty()) 
			{
				if (!safeEdge(aln.edge)) continue;
				readStart = aln.overlap.curEnd + aln.overlap.extLen - 
							aln.overlap.extEnd;
				readStart = std::min(readStart, aln.overlap.curLen - MAGIC_100);
			}

			currentAln.push_back(aln);
			if (safeEdge(aln.edge) && currentAln.front().edge != aln.edge)
			{
				bool reliableConnection = true;

				//if any of the edges does not prevent contig extenstion, 
				//no need to resolve it
				if (!currentAln.front().edge->nodeRight->isBifurcation() ||
					!currentAln.back().edge->nodeLeft->isBifurcation()) reliableConnection = false;

				//don't connect edges if they both were previously repetitive
				//(end then became unique)
				if (currentAln.front().edge->resolved &&
					currentAln.back().edge->resolved) reliableConnection = false;

				//don't connect edges, if they are already linked
				//(through a alternative haplotypes structure)
				if (currentAln.front().edge->rightLink || 
					currentAln.back().edge->leftLink) reliableConnection = false;

				if (!reliableConnection)
				{
					currentAln.clear();
					currentAln.push_back(aln);
					readStart = aln.overlap.curEnd + aln.overlap.extLen - 
								aln.overlap.extEnd;
					readStart = std::min(readStart, aln.overlap.curLen - MAGIC_100);
					continue;
				}

				int32_t flankScore = std::min(currentAln.front().overlap.curRange(),
											  currentAln.back().overlap.curRange());
				GraphPath currentPath;
				for (auto& aln : currentAln) currentPath.push_back(aln.edge);
				GraphPath complPath = _graph.complementPath(currentPath);

				int32_t readEnd = aln.overlap.curBegin - aln.overlap.extBegin;

				//TODO: fix this ad-hoc fix. Currently, if read connects
				//two consecutive edges (for example, when resolving chimera junctions,
				//we still would insert a tiny bit of read sequence as a placeholder.
				//Probably, wouldn't hurt, but who knows..
				readEnd = std::max(readStart + MAGIC_100 - 1, readEnd);	
				if (readStart < 0 || readEnd >= aln.overlap.curLen)
				{
					Logger::get().warning() 
						<< "Something is wrong with bridging read sequence";
					//Logger::get().warning() << readStart << " " 
					//	<< readEnd << " " << aln.overlap.curLen;
					break;
				}

				ReadSequence readSeq = {aln.overlap.curId, readStart, readEnd};
				ReadSequence complRead = {aln.overlap.curId.rc(), 
										  aln.overlap.curLen - readEnd - 1,
										  aln.overlap.curLen - readStart - 1};
				readConnections.push_back({currentPath, readSeq, flankScore});
				readConnections.push_back({complPath, complRead, flankScore});

				currentAln.clear();
				currentAln.push_back(aln);
				readStart = aln.overlap.curEnd + aln.overlap.extLen - 
							aln.overlap.extEnd;
				readStart = std::min(readStart, aln.overlap.curLen - MAGIC_100);
			}
		}
	}

	return readConnections;
}

//cleans up the graph after repeat resolution
void RepeatResolver::clearResolvedRepeats()
{
	//const int MIN_LOOP = Parameters::get().minimumOverlap;
	auto nextEdge = [](GraphNode* node)
	{
		for (auto edge : node->outEdges)
		{
			if (!edge->isLooped()) return edge;
		}
		return (GraphEdge*)nullptr;
	};

	auto shouldRemove = [](GraphEdge* edge)
	{
		//return edge->isRepetitive() && edge->resolved;
		return edge->resolved;
	};

	std::unordered_set<GraphNode*> toRemove;

	for (auto& node : _graph.iterNodes())
	{
		//separated nodes
		if (node->neighbors().size() == 0)
		{
			bool resolved = true;
			for (auto& edge : node->outEdges) 
			{
				if (!shouldRemove(edge)) resolved = false;
			}

			if (resolved) toRemove.insert(node);
		}

		//other nodes
		if (!node->isEnd()) continue;

		GraphEdge* direction = nextEdge(node);
		if (!direction) continue;

		GraphPath traversed;
		traversed.push_back(direction);
		GraphNode* curNode = direction->nodeRight;
		while (curNode->isResolved())
		{
			traversed.push_back(nextEdge(curNode));
			curNode = traversed.back()->nodeRight;
		}
		if (traversed.empty()) continue;

		bool removeLast = curNode->isEnd();
		bool resolvedRepeat = true;
		for (auto& edge : traversed) 
		{
			if (!shouldRemove(edge)) resolvedRepeat = false;
		}

		GraphPath complPath = _graph.complementPath(traversed);
		if (resolvedRepeat)
		{
			//first-last
			toRemove.insert(traversed.front()->nodeLeft);
			if (removeLast) toRemove.insert(complPath.front()->nodeLeft);

			//middle nodes
			for (size_t i = 0; i < traversed.size() - 1; ++i)
			{
				toRemove.insert(traversed[i]->nodeRight);
				toRemove.insert(complPath[i]->nodeRight);
			}

			//last-first
			if (removeLast) toRemove.insert(traversed.back()->nodeRight);
			toRemove.insert(complPath.back()->nodeRight);
		}
	}

	for (auto node : toRemove) _graph.removeNode(node);
	_aligner.updateAlignments();
}


int RepeatResolver::resolveSimpleRepeats()
{
	static const int MIN_JCT_SUPPORT = 2;

	auto alnIndex = _aligner.makeAlignmentIndex();

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();
	int candidateEdges = 0;

	std::vector<Connection> resolvedConnections;
	for (auto& pathToResolve : unbranchingPaths)
	{
		if (!pathToResolve.id.strand()) continue;
		if (pathToResolve.path.front()->selfComplement) continue;

		std::unordered_set<GraphEdge*> inputs(pathToResolve.nodeLeft()->inEdges.begin(),
											  pathToResolve.nodeLeft()->inEdges.end());
		std::unordered_set<GraphEdge*> outputs(pathToResolve.nodeRight()->outEdges.begin(),
											   pathToResolve.nodeRight()->outEdges.end());
		if (pathToResolve.nodeLeft()->outEdges.size() != 1 ||
			pathToResolve.nodeRight()->inEdges.size() != 1 ||
			inputs.size() != outputs.size() ||
			inputs.size() <= 1) continue;

		++candidateEdges;
		std::unordered_map<GraphEdge*, 
					   	   std::unordered_map<GraphEdge*, int>> connections;
		std::unordered_map<GraphEdge*, 
					   	   std::unordered_map<GraphEdge*, ReadSequence>> bridgingReads;
		for (GraphEdge* inEdge : inputs)
		{
			for (auto& aln : alnIndex[inEdge])
			{
				for (size_t i = 0; i < aln.size(); ++i)
				{
					if (aln[i].edge != inEdge) continue;
					for (size_t j = i + 1; j < aln.size(); ++j)
					{
						if (outputs.count(aln[j].edge))
						{
							++connections[inEdge][aln[j].edge];
							bridgingReads[inEdge][aln[j].edge] = 
								{aln[i].overlap.curId, aln[i].overlap.curEnd, 
								 aln[j].overlap.curBegin};
							break;
						}
					}
				}
			}
		}

		//initializing sets (to cluster them later)
		typedef SetNode<GraphEdge*> SetElement;
		std::unordered_map<GraphEdge*, SetElement*> edgeToElement;
		SetVec<GraphEdge*> allElements;
		for (GraphEdge* edge : inputs) 
		{
			allElements.push_back(new SetElement(edge));
			edgeToElement[edge] = allElements.back();
		}
		for (GraphEdge* edge : outputs) 
		{
			allElements.push_back(new SetElement(edge));
			edgeToElement[edge] = allElements.back();
		}

		//grouping edges if they are connected by reads
		for (GraphEdge* inEdge : inputs)
		{
			for (auto outEdge : connections[inEdge])
			{
				if (outEdge.second >= MIN_JCT_SUPPORT)
				{
					unionSet(edgeToElement[inEdge], 
							 edgeToElement[outEdge.first]);
				}
			}
		}

		auto clusters = groupBySet(allElements);
		/*if (clusters.size() > 1)
		{
			Logger::get().debug() << "Split edge mult:" 
				<< inputs.size() << "len: " << pathToResolve.length
				<< " cov: " << pathToResolve.meanCoverage 
				<< " clusters: " << clusters.size();
			for (auto& cl : clusters)
			{
				Logger::get().debug() << "\tCl: " << cl.second.size();
				for (auto edge : cl.second)
				{
					Logger::get().debug() << "\t\t" << edge->edgeId.signedId() << " " 
						<< edge->length() << " " << edge->meanCoverage 
						<< " " << inputs.count(edge);
				}
			}
		}*/
		for (auto& cl : clusters)
		{
			GraphEdge* inputConn = nullptr;
			GraphEdge* outputConn = nullptr;
			if (cl.second.size() == 2)
			{
				for (size_t i = 0; i < 2; ++i)
				{
					if (inputs.count(cl.second[i])) inputConn = cl.second[i];
					if (outputs.count(cl.second[i])) outputConn = cl.second[i];
				}
			}
			//TODO: allow resolving loops (e.g. same input and output)
			if (inputConn && outputConn &&
				inputConn != outputConn)
			{
				GraphPath connPath;
				connPath.push_back(inputConn);
				connPath.insert(connPath.end(), pathToResolve.path.begin(), 
								pathToResolve.path.end());
				connPath.push_back(outputConn);
				resolvedConnections.push_back({connPath, 
											   bridgingReads[inputConn][outputConn],
											   /*flnak len*/ 0});
			}
		}
	}

	//separate repeats on the graph
	for (auto& conn : resolvedConnections)
	{
		Logger::get().debug() << "\tConnection " 
			<< conn.path.front()->edgeId.signedId() << "\t" 
			<< conn.path.back()->edgeId.signedId();
		FastaRecord::Id edgeId = _graph.newEdgeId();

		std::stringstream ss;
		ss << "edge_" << edgeId.signedId() << "_0_" 
			<< _readSeqs.getRecord(conn.readSeq.readId).description << "_"
			<< conn.readSeq.start << "_" << conn.readSeq.end;
		EdgeSequence edgeSeq = 
			_graph.addEdgeSequence(_readSeqs.getSeq(conn.readSeq.readId),
								   conn.readSeq.start, conn.readSeq.length(),
								   ss.str());

		_graph.separatePath(conn.path, edgeSeq, edgeId);
		_graph.separatePath(_graph.complementPath(conn.path), 
						    edgeSeq.complement(), edgeId.rc());
	}

	Logger::get().debug() << "[SIMPL] Resolved " << resolvedConnections.size() 
		<< " simple repeats";
	_aligner.updateAlignments();
	return resolvedConnections.size();
}
